# SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause)
DEPS	     := ./deps

SRC_DIR ?= src
USER_SRC_DIR ?= ${SRC_DIR}/user
KERNEL_SRC_DIR ?= ${SRC_DIR}/kernel

XDP_PROGS    := xdp_geneve xdp_redirect
XDP_TARGETS  := ${XDP_PROGS:=.o}
USER_TARGETS := ${XDP_PROGS:=_user}
USER_LIBS    := -lbpf -lm

$(info XDP_TARGETS is [${XDP_TARGETS}])
$(info USER_TARGETS is [${USER_TARGETS}])

LIBBPF_DIR  = ${DEPS}/libbpf/src
COMMON_DIR  = ${DEPS}/common
HEADERS_DIR = ${DEPS}/headers

EXTRA_DEPS  += $(COMMON_DIR)/parsing_helpers.h

include $(COMMON_DIR)/common.mk

.PHONY: tags
tags:
	ctags -e -R
