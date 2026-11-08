APITESTPROGS-$(call ALLYES, LIBASTCENC_ENCODER LIBASTCENC_DECODER) += api-astc-profile
APITESTPROGS-$(call ENCDEC, FLAC, FLAC) += api-flac
APITESTPROGS-$(call DEMDEC, H264, H264) += api-h264
APITESTPROGS-$(call DEMDEC, H264, H264) += api-h264-slice
APITESTPROGS-$(CONFIG_VMIX_DECODER) += api-vmix
APITESTPROGS-$(call ALLYES, OMT_DEMUXER OMT_MUXER TCP_PROTOCOL) += api-omt
APITESTPROGS-$(CONFIG_MOV_MUXER)        += api-movenc
APITESTPROGS-$(CONFIG_MOVTEXT_ENCODER)  += api-movtext
APITESTPROGS-$(call ALLYES, HEVC_MP4TOANNEXB_BSF HEVC_DECODER) += api-hevc-mp4toannexb
APITESTPROGS-yes += api-seek api-dump-stream-meta
APITESTPROGS-$(call DEMDEC, H263, H263) += api-band
APITESTPROGS-$(HAVE_THREADS) += api-threadmessage
APITESTPROGS-$(CONFIG_AVCODEC) += api-enc-info
APITESTPROGS-$(call ALLYES, H261_ENCODER H261_PARSER) += api-enc-parser
ifneq ($(strip $(LIBVMX_ROOT)),)
APITESTPROGS-yes += api-vmix-bench
$(APITESTSDIR)/api-vmix-bench-test.o: CPPFLAGS += -I$(LIBVMX_ROOT)/src
$(APITESTSDIR)/api-vmix-bench-test$(EXESUF): ELIBS += -L$(LIBVMX_ROOT)/build -Wl,-rpath,$(LIBVMX_ROOT)/build -lvmx
endif
ifneq ($(strip $(LIBOMT_ROOT)),)
APITESTPROGS-yes += api-omt-bench
$(APITESTSDIR)/api-omt-bench-test.o: CPPFLAGS += -isystem $(LIBOMT_ROOT)
$(APITESTSDIR)/api-omt-bench-test$(EXESUF): ELIBS += -L$(LIBOMT_ROOT)/bin/Release/net8.0/linux-x64/native -Wl,-rpath,$(LIBOMT_ROOT)/bin/Release/net8.0/linux-x64/native -lomt
endif
APITESTPROGS += $(APITESTPROGS-yes)

APITESTOBJS  := $(APITESTOBJS:%=$(APITESTSDIR)%) $(APITESTPROGS:%=$(APITESTSDIR)/%-test.o)
APITESTPROGS := $(APITESTPROGS:%=$(APITESTSDIR)/%-test$(EXESUF))
-include $(wildcard $(APITESTOBJS:.o=.d))

$(APITESTOBJS): | $(sort $(dir $(APITESTOBJS)))
$(APITESTOBJS) $(APITESTOBJS:.o=.i): CPPFLAGS += -DTEST

$(APITESTPROGS): %$(EXESUF): %.o $(FF_DEP_LIBS)
	$(call LINK,$(LDFLAGS) $(LDEXEFLAGS) $(LD_O) $(filter %.o,$^) $(FF_EXTRALIBS) $(ELIBS))

testclean::
	$(RM) $(addprefix $(APITESTSDIR)/,$(CLEANSUFFIXES) *-test$(EXESUF))
