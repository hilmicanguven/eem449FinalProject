# invoke SourceDir generated makefile for httpget.pem4f
httpget.pem4f: .libraries,httpget.pem4f
.libraries,httpget.pem4f: package/cfg/httpget_pem4f.xdl
	$(MAKE) -f C:\Users\hilmi\Desktop\tcp_client_v4/src/makefile.libs

clean::
	$(MAKE) -f C:\Users\hilmi\Desktop\tcp_client_v4/src/makefile.libs clean

