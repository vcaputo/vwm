vwm:	vwm.c vwm.h list.h colors.def launchers.def Makefile
	$(CC) -Wall -o vwm vwm.c -lX11 #-g -DTRACE

clean:
	rm -f vwm
