# SPDX-License-Identifier: GPL-2.0
# conn_md, the LTE coex (IDC) message bridge between WMT and the modem, from
# the 4.14 MediaTek tree. It only needs eccci's IPC
# headers (struct ipc_ilm, the module IDs), not eccci itself.

ccflags-y += -I$(srctree)/drivers/misc/mediatek/eccci/$(MTK_PLATFORM)
ccflags-y += -I$(srctree)/drivers/misc/mediatek/eccci/
ccflags-y += -I$(srctree)/$(src)/include/

obj-y += conn_md_bridge.o
conn_md_bridge-objs := conn_md.o
conn_md_bridge-objs += conn_md_exp.o
conn_md_bridge-objs += conn_md_log.o
conn_md_bridge-objs += conn_md_dump.o
conn_md_bridge-objs += conn_md_dbg.o
conn_md_bridge-objs += conn_md_test.o
