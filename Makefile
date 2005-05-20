ifndef NAVISERVER
    NAVISERVER  = /usr/local/ns
endif

#
# Module name
#
MOD      =  nsaspell.so

#
# Objects to build.
#
OBJS     = nsaspell.o

MODLIBS	 = -laspell

include  $(NAVISERVER)/include/Makefile.module
