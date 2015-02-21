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
MODOBJS     = nsaspell.o

MODLIBS	 = -laspell

include  $(NAVISERVER)/include/Makefile.module
