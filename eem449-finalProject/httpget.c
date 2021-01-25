
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <xdc/std.h>  //adc
// XDCtools Header files
#include <xdc/runtime/Error.h>
#include <xdc/runtime/System.h>

/* TI-RTOS Header files */
#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Swi.h>
#include <ti/sysbios/knl/Queue.h>
#include <ti/sysbios/knl/Event.h>
#include <ti/sysbios/knl/Idle.h>
#include <ti/sysbios/knl/Mailbox.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <ti/drivers/GPIO.h>
#include <ti/net/http/httpcli.h>

#include "Board.h"

#include <sys/socket.h>
#include <arpa/inet.h>

// new headers---ADC
#include <stdbool.h>
#include "inc/hw_memmap.h"
#include "inc/hw_ints.h"
#include "inc/hw_types.h"
#include "driverlib/adc.h"
#include "driverlib/gpio.h"
#include "driverlib/sysctl.h"

//#define HOSTNAME         "api.openweathermap.org"
//#define HOSTNAME        "http://api.weatherbit.io/v2.0"
#define HOSTNAME          "api.weatherbit.io"
#define REQUEST_URI       "/v2.0/current?lat=47.7796&lon=15.6382&key=aaec4504942343999b628ce0b1cf3c78"

#define USER_AGENT        "HTTPCli (ARM; TI-RTOS)"
#define SOCKETTEST_IP     "192.168.1.28"  //ipconfig ile bulduðum benim IP adresim
#define TASKSTACKSIZE     4096
#define OUTGOING_PORT     5011
#define INCOMING_PORT     5030

extern Semaphore_Handle semaphore0;     // posted by httpTask and pended by clientTask
char   tempstr[40];                     // temperature string
char   weatherString[40];
extern Swi_Handle swi0;
extern Mailbox_Handle mailbox0;
extern Event_Handle event0;
#define IP                "132.163.97.5"
char dataTime[4];

char message[100];
char timeMessage[100];
uint32_t ADCValues[2];
int sensorValue = 0;

Void timerHWI(UArg arg1)
{
    //
    // Just trigger the ADC conversion for sequence 3. The rest will be done in SWI
    //
    ADCProcessorTrigger(ADC0_BASE, 3);

    // post the SWI for the rest of ADC data conversion and buffering
    //
    Swi_post(swi0);
}
Void ADCSwi(UArg arg1, UArg arg2)
{
    static uint32_t PE3_value;

    //
    // Wait for conversion to be completed for sequence 3
    //
    while(!ADCIntStatus(ADC0_BASE, 3, false));

    //
    // Clear the ADC interrupt flag for sequence 3
    //
    ADCIntClear(ADC0_BASE, 3);

    //
    // Read ADC Value from sequence 3
    //
    ADCSequenceDataGet(ADC0_BASE, 3, ADCValues);

    //
    // Port E Pin 3 is the AIN0 pin. Therefore connect PE3 pin to the line that you want to
    // acquire. +3.3V --> 4095 and 0V --> 0
    //
    PE3_value = ADCValues[0]; // PE3 : Port E pin 3

    // send the ADC PE3 values to the taskAverage()
    //
    Mailbox_post(mailbox0, &PE3_value, BIOS_NO_WAIT);
}

Void taskAverage(UArg arg1, UArg arg2)
{

    static uint32_t pe3_val, pe3_average, tot=0;
    int i;
    int iter=0;

    while(1){

        tot = 0;                // clear total ADC values
        Event_pend(event0, Event_Id_00,Event_Id_00,BIOS_WAIT_FOREVER);
        for(i=0;i<1;i++) {     // 10 ADC values will be retrieved

            // wait for the mailbox until the buffer is full
            //
            //Mailbox_pend(mailbox0, &pe3_val, BIOS_WAIT_FOREVER);
            Mailbox_pend(mailbox0, &pe3_val, BIOS_WAIT_FOREVER);
            tot += pe3_val;
        }
        pe3_average = tot /1;

        System_printf("Average: %d\n", pe3_average);
        sensorValue = pe3_average;
        System_flush();

        // After 10 iterations, we will quit and Console shows the results
        //
        iter++;
        //if(iter == 3) {
          //  BIOS_exit(-1);  // quit from TI-RTOS
        //}

    }
}

void initialize_ADC()
{
    // enable ADC and Port E
    //
    SysCtlPeripheralReset(SYSCTL_PERIPH_ADC0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_ADC0);
    SysCtlPeripheralReset(SYSCTL_PERIPH_GPIOE);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOE);
    SysCtlDelay(10);

    // Select the analog ADC function for Port E pin 3 (PE3)
    //
    GPIOPinTypeADC(GPIO_PORTE_BASE, GPIO_PIN_3);

    // configure sequence 3
    //
    ADCSequenceConfigure(ADC0_BASE, 3, ADC_TRIGGER_PROCESSOR, 0);

    // every step, only PE3 will be acquired
    //
    ADCSequenceStepConfigure(ADC0_BASE, 3, 0, ADC_CTL_CH0 | ADC_CTL_IE | ADC_CTL_END);

    // Since sample sequence 3 is now configured, it must be enabled.
    //
    ADCSequenceEnable(ADC0_BASE, 3);

    // Clear the interrupt status flag.  This is done to make sure the
    // interrupt flag is cleared before we sample.
    //
    ADCIntClear(ADC0_BASE, 3);
}

/*
 *  ======== printError ========
 */
void printError(char *errString, int code)
{
    System_printf("Error! code = %d, desc = %s\n", code, errString);
    BIOS_exit(code);
}

bool sendData2Server(char *serverIP, int serverPort, char *data, int size)
{
    int sockfd, connStat, numSend;
    bool retval=false;
    struct sockaddr_in serverAddr;

    sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd == -1) {
        System_printf("Socket not created");
        close(sockfd);
        return false;
    }

    memset(&serverAddr, 0, sizeof(serverAddr));  // clear serverAddr structure
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(serverPort);     // convert port # to network order
    inet_pton(AF_INET, serverIP, &(serverAddr.sin_addr));

    connStat = connect(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr));
    if(connStat < 0) {
        System_printf("sendData2Server::Error while connecting to server\n");
    }
    else {

        numSend = send(sockfd, data, size, 0);       // send data to the server
        if(numSend < 0) {
            System_printf("problem!!sendData2Server::Error while sending data to server\n");
        }
        else {
            //System_printf("retval true oldu");
            retval = true;      // we successfully sent the temperature string
        }
    }
    System_flush();
    close(sockfd);
    return retval;
}
void getTime()
{

        int sockfd, connStat;
        struct sockaddr_in serverAddr;
        sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sockfd == -1) {
            System_printf("Socket not created");
            BIOS_exit(-1);

        }
        memset(&serverAddr, 0, sizeof(serverAddr));  // clear serverAddr structure
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(37);     // convert port # to network order
        inet_pton(AF_INET, IP , &(serverAddr.sin_addr));
        connStat = connect(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr));
        if(connStat < 0) {
            System_printf("sendData2Server::Error while connecting to server\n");
            if(sockfd>0) close(sockfd);
            BIOS_exit(-1);
        }
        System_flush();
        recv(sockfd, dataTime, 4, 0);

        unsigned long int seconds= dataTime[0]*16777216 +  dataTime[1]*65536 + dataTime[2]*256 + dataTime[3];
        seconds  += 10800;
        char* buf = ctime(&seconds);
        //buf = ctime(&seconds);
        //printf("the time is %s", buf+7);
        //strcat(message,buf);
        uint8_t i;


        for(i = 0; i < 27; i++)
        {
            timeMessage[i] = buf[i];
        }

        System_printf("the time is %s\n", buf);
        System_printf("the message is: %s\n", timeMessage);
        System_flush();



        if(sockfd>0) close(sockfd);


}

Void clientSocketTask(UArg arg0, UArg arg1) //client task
{
    while(1) {
        // wait for the semaphore that httpTask() will signal
        // when temperature string is retrieved from api.openweathermap.org site
        //
        Semaphore_pend(semaphore0, BIOS_WAIT_FOREVER);

        char message[40] = "";
        GPIO_write(Board_LED0, 1); // turn on the LED

        System_printf("\nWeatherCode : %s \n", weatherString);
        System_flush();

        getTime();
        strcat(message,"message start*** ");
        strcat(message,timeMessage);
        strcat(message," ");
        strcat(message,weatherString);
        strcat(message," and rainy. message finish*** ");
        strcat(message,"\n");

        if(sendData2Server(SOCKETTEST_IP, OUTGOING_PORT, message, strlen(message))) {
            System_printf("clientSocketTask:: Temperature is sent to the server\n");
            System_flush();
        }

        GPIO_write(Board_LED0, 0);  // turn off the LED
    }
}

void getTimeStr(char *str)
{
    // dummy get time as string function
    // you may need to replace the time by getting time from NTP servers
    //
    strcpy(str, "2021-01-07 12:34:56");
}

float getTemperature(void)
{
    // dummy return
    //
    return atof(weatherString);
}

Void serverSocketTask(UArg arg0, UArg arg1) //server task
{
    int serverfd, new_socket, valread, len;
    struct sockaddr_in serverAddr, clientAddr;
    float temp;
    char buffer[30];
    char outstr[30], tmpstr[30];
    bool quit_protocol;

    serverfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverfd == -1) {
        System_printf("serverSocketTask::Socket not created.. quiting the task.\n");
        return;     // we just quit the tasks. nothing else to do.
    }

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(INCOMING_PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    // Attaching socket to the port
    //
    if (bind(serverfd, (struct sockaddr *)&serverAddr,  sizeof(serverAddr))<0) {
         System_printf("serverSocketTask::bind failed..\n");

         // nothing else to do, since without bind nothing else works
         // we need to terminate the task
         return;
    }
    if (listen(serverfd, 3) < 0) {

        System_printf("serverSocketTask::listen() failed\n");
        // nothing else to do, since without bind nothing else works
        // we need to terminate the task
        return;
    }

    while(1) {

        len = sizeof(clientAddr);
        if ((new_socket = accept(serverfd, (struct sockaddr *)&clientAddr, &len))<0) {
            System_printf("serverSocketTask::accept() failed\n");
            continue;               // get back to the beginning of the while loop
        }

        System_printf("Accepted connection\n"); // IP address is in clientAddr.sin_addr
        System_flush();

        // task while loop
        //
        quit_protocol = false;
        do {

            // let's receive data string
            if((valread = recv(new_socket, buffer, 10, 0))<0) {

                // there is an error. Let's terminate the connection and get out of the loop
                //
                close(new_socket);
                break;
            }

            // let's truncate the received string
            //
            buffer[10]=0;   //sonuna sýfýr koyuyoruz
            if(valread<10) buffer[valread]=0;

            System_printf("message received: %s\n", buffer);

            if(!strcmp(buffer, "HELLO")) {
                strcpy(outstr,"hGREETINGS 200\n");
                send(new_socket , outstr , strlen(outstr) , 0);
                System_printf("Server <-- GREETINGS 200\n");
            }
            else if(!strcmp(buffer, "GETTIME")) {
                getTimeStr(tmpstr);
                strcpy(outstr, "OKeyy: ");
                strcat(outstr, tmpstr);
                strcat(outstr, "\n");
                send(new_socket , outstr , strlen(outstr) , 0);
            }
            else if(!strcmp(buffer, "GETTEMP")) {
                temp = getTemperature();
                sprintf(outstr, "OK %5.2f\n", temp);
                send(new_socket , outstr , strlen(outstr) , 0);
            }
            else if(!strcmp(buffer, "QUIT")) {
                quit_protocol = true;     // it will allow us to get out of while loop
                strcpy(outstr, "BYE 200");
                send(new_socket , outstr , strlen(outstr) , 0);
            }

        }
        while(!quit_protocol);

        System_flush();
        close(new_socket);
    }

    close(serverfd);
    return;
}

/*
 *  ======== httpTask ========
 *  Makes a HTTP GET request
 */
Void httpTask(UArg arg0, UArg arg1)
{
    bool moreFlag = false;
    char data[64], *s1, *s2;
    int ret, temp_received=0, len;
    struct sockaddr_in addr;

    HTTPCli_Struct cli;
    HTTPCli_Field fields[3] = {
        { HTTPStd_FIELD_NAME_HOST, HOSTNAME },
        { HTTPStd_FIELD_NAME_USER_AGENT, USER_AGENT },
        { NULL, NULL }
    };

    while(1) {

        Event_post(event0, Event_Id_00);

        System_printf("Sending a HTTP GET request to '%s'\n", HOSTNAME);
        System_flush();

        HTTPCli_construct(&cli);

        HTTPCli_setRequestFields(&cli, fields);

        ret = HTTPCli_initSockAddr((struct sockaddr *)&addr, HOSTNAME, 0);
        if (ret < 0) {
            HTTPCli_destruct(&cli);
            System_printf("httpTask: address resolution failed\n");
            continue;
        }

        ret = HTTPCli_connect(&cli, (struct sockaddr *)&addr, 0, NULL);
        if (ret < 0) {
            HTTPCli_destruct(&cli);
            System_printf("httpTask: connect failed\n");
            continue;
        }

        ret = HTTPCli_sendRequest(&cli, HTTPStd_GET, REQUEST_URI, false);
        if (ret < 0) {
            HTTPCli_disconnect(&cli);
            HTTPCli_destruct(&cli);
            System_printf("httpTask: send failed");
            continue;
        }

        ret = HTTPCli_getResponseStatus(&cli);
        if (ret != HTTPStd_OK) {
            HTTPCli_disconnect(&cli);
            HTTPCli_destruct(&cli);
            System_printf("httpTask: cannot get status\n");
            continue;
        }

        System_printf("HTTP Response Status Code: %d\n", ret);

        ret = HTTPCli_getResponseField(&cli, data, sizeof(data), &moreFlag);
        if (ret != HTTPCli_FIELD_ID_END) {
            HTTPCli_disconnect(&cli);
            HTTPCli_destruct(&cli);
            System_printf("httpTask: response field processing failed\n");
            continue;
        }

        len = 0;
        do {
            ret = HTTPCli_readResponseBody(&cli, data, sizeof(data), &moreFlag);
            if (ret < 0) {
                HTTPCli_disconnect(&cli);
                HTTPCli_destruct(&cli);
                System_printf("httpTask: response body processing failed\n");
                moreFlag = false; //eðer hata verdiyse eb içteki whiledan de çýk
            }
            else {
                // string is read correctly
                // find "temp:" string
                //
                //System_printf("data is %s \n",data);
                //System_flush();

                s1=strstr(data, "code");
                //s1=strstr(s1,"code");
                //System_printf("vode:  %s\n", s1);
                if(s1) {
                    if(temp_received) continue;     // temperature is retrieved before, continue
                    // is s1 is not null i.e. "temp" string is found
                    // search for comma
                    s2=strstr(s1, ",");
                    if(s2) {
                        *s2=0;                      // put end of string
                        strcpy(weatherString, s1+6);      // copy the string
                        temp_received = 1;
                    }
                }
            }

            len += ret;     // update the total string length received so far
        } while (moreFlag);

        //System_printf("data is %s \n",data);
        System_printf("Received %d bytes of payload\n", len);
        System_printf("weather code is  %s\n", weatherString);
        System_flush();                                         // write logs to console

        HTTPCli_disconnect(&cli);                               // disconnect from openweathermap
        HTTPCli_destruct(&cli);


        int weatherCodeValue = atol(weatherString);
        System_printf("WEather Code is:  %d\n", weatherCodeValue);
        if((weatherCodeValue  < 601)  ){
            if(sensorValue > 3500 ){
                System_printf("value sent to the socket task");
                Semaphore_post(semaphore0);
            }
            //Semaphore_post(semaphore0);                             // activate socketTask
        }
        Task_sleep(5000);                                       // sleep 5 seconds
    }
}

bool createTasks(void)
{
    static Task_Handle taskHandle1, taskHandle2, taskHandle3;
    Task_Params taskParams;
    Error_Block eb;

    Error_init(&eb);

    Task_Params_init(&taskParams);
    taskParams.stackSize = TASKSTACKSIZE;
    taskParams.priority = 1;
    taskHandle1 = Task_create((Task_FuncPtr)httpTask, &taskParams, &eb);

    Task_Params_init(&taskParams);
    taskParams.stackSize = TASKSTACKSIZE;
    taskParams.priority = 1;
    taskHandle2 = Task_create((Task_FuncPtr)clientSocketTask, &taskParams, &eb);

    Task_Params_init(&taskParams);
    taskParams.stackSize = TASKSTACKSIZE;
    taskParams.priority = 1;
    taskHandle3 = Task_create((Task_FuncPtr)serverSocketTask, &taskParams, &eb);

    if (taskHandle1 == NULL || taskHandle2 == NULL || taskHandle3 == NULL) {
        printError("netIPAddrHook: Failed to create HTTP, Socket and Server Tasks\n", -1);
        return false;
    }

    return true;
}

//  This function is called when IP Addr is added or deleted
//
void netIPAddrHook(unsigned int IPAddr, unsigned int IfIdx, unsigned int fAdd)
{
    // Create a HTTP task when the IP address is added
    //her yeni IP adresi alýndýðýnda bu çalýþýr ve task oluþturur.
    if (fAdd) {
        createTasks();
    }
}

int main(void)
  {
    /* Call board init functions */
    Board_initGeneral();
    Board_initGPIO();
    Board_initEMAC();

    /* Turn on user LED */
    GPIO_write(Board_LED0, Board_LED_ON);

    System_printf("Starting the HTTP GET example\nSystem provider is set to "
            "SysMin. Halt the target to view any SysMin contents in ROV.\n");
    /* SysMin will only print to the console when you call flush or exit */
    System_flush();

    // this function initializes PE3 as ADC input.
        //
    initialize_ADC();
    /* Start BIOS */
    BIOS_start();

    return (0);
}
