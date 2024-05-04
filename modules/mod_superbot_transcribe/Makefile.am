include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_superbot_transcribe

mod_LTLIBRARIES = mod_superbot_transcribe.la
mod_superbot_transcribe_la_SOURCES  = mod_superbot_transcribe.c superbot_glue.cpp 
mod_superbot_transcribe_la_CFLAGS   = $(AM_CFLAGS)
mod_superbot_transcribe_la_CXXFLAGS = -I $(top_srcdir)/libs/superbot-asr-grpc-api/stubs $(AM_CXXFLAGS) -std=c++17

mod_superbot_transcribe_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_superbot_transcribe_la_LDFLAGS  = -avoid-version -module -no-undefined -shared `pkg-config --libs grpc++ grpc`

