# vcigb: e1000/igb with VeloCloud Edge 520/540 board support, as a
# standalone module next to the built-in em/igb driver.
#
# Build on FreeBSD with kernel sources matching the target kernel, choosing
# the per-kernel tree with K (default: k1600018, pfSense 2.9.0):
#   make SYSDIR=/usr/src/sys K=k1600018
K?=	k1600018
.PATH:	${.CURDIR}/${K}/e1000
KMOD=	vcigb
SRCS=	device_if.h bus_if.h pci_if.h ifdi_if.h \
	opt_ddb.h opt_inet.h opt_inet6.h opt_rss.h
SRCS+=	if_em.c em_txrx.c igb_txrx.c e1000_osdep.c
SRCS+=	e1000_api.c e1000_base.c e1000_phy.c e1000_nvm.c e1000_mac.c \
	e1000_manage.c e1000_vf.c e1000_mbx.c e1000_i210.c \
	e1000_80003es2lan.c e1000_ich8lan.c e1000_82571.c e1000_82575.c \
	e1000_82540.c e1000_82542.c e1000_82541.c e1000_82543.c
SRCS+=	e1000_vc.c
CFLAGS+= -I${.CURDIR}/${K}/e1000

.include <bsd.kmod.mk>
