CFLAGS=-O2 -D_GNU_SOURCE #-g

vwm:		libvmon/vmon.o	\
		vwm.c		\
		vwm.h		\
		list.h		\
		colors.def	\
		launchers.def	\
		Makefile
	$(CC) $(CFLAGS) -Wall -o vwm vwm.c -I. libvmon/vmon.o -lX11 -lXext -lXinerama -lXrandr -lXcomposite -lXdamage -lXrender -lXfixes #-DTRACE

libvmon/vmon.o:	Makefile			\
		list.h				\
		libvmon/bitmap.h		\
		libvmon/vmon.h			\
		libvmon/vmon.c			\
		libvmon/defs/_begin.def		\
		libvmon/defs/_end.def		\
		libvmon/defs/sys_stat.def	\
		libvmon/defs/sys_vm.def		\
		libvmon/defs/proc_stat.def	\
		libvmon/defs/proc_vm.def	\
		libvmon/defs/proc_io.def	\
		libvmon/defs/proc_files.def	\
		libvmon/defs/sys_wants.def	\
		libvmon/defs/proc_wants.def
	$(CC) $(CFLAGS) -c -o libvmon/vmon.o libvmon/vmon.c -I. -Ilibvmon

clean:
	rm -f vwm libvmon/vmon.o
