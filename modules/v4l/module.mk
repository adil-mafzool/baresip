#
# module.mk
#
# Copyright (C) 2010 Creytiv.com
#

MOD		:= v4l
$(MOD)_SRCS	+= v4l.c
$(MOD)_LFLAGS	+= -lavformat -lswscale

include mk/mod.mk
