noinst_LTLIBRARIES = libcfnet.la

AM_CPPFLAGS =  -I$(top_srcdir)/libutils				# platform.h
AM_CPPFLAGS += -I$(top_srcdir)/libpromises			# cf3.defs.h

libcfnet_la_SOURCES = \
	misc.c \
	net.c net.h \
        communication.c communication.h \
	client_protocol.c client_protocol.h \
	client_code.c client_code.h
