/*	$NetBSD: s390.c,v 1.7 2021/01/30 17:38:57 thorpej Exp $	*/

/*-
 * Copyright (c) 2011 Frank Wille.
 * All rights reserved.
 *
 * Written by Frank Wille for The NetBSD Project.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: s390.c,v 1.7 2021/01/30 17:38:57 thorpej Exp $");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/conf.h>

#include <dev/clock_subr.h>

#include <dev/i2c/i2cvar.h>
#include <dev/i2c/s390reg.h>

struct s390rtc_softc {
	device_t sc_dev;
	i2c_tag_t sc_tag;
	i2c_addr_t sc_addr;
	struct todr_chip_handle sc_todr;
};

static int s390rtc_match(device_t, cfdata_t, void *);
static void s390rtc_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(s390rtc, sizeof(struct s390rtc_softc),
    s390rtc_match, s390rtc_attach, NULL, NULL);

static int s390rtc_gettime_ymdhms(struct todr_chip_handle *,
				  struct clock_ymdhms *);
static int s390rtc_settime_ymdhms(struct todr_chip_handle *,
				  struct clock_ymdhms *);
static int s390rtc_read(struct s390rtc_softc *, int, uint8_t *, size_t);
static int s390rtc_write(struct s390rtc_softc *, int, uint8_t *, size_t);
static uint8_t bitreverse(uint8_t);

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "sii,s35390a" },
	DEVICE_COMPAT_EOL
};

static int
s390rtc_match(device_t parent, cfdata_t cf, void *arg)
{
	struct i2c_attach_args *ia = arg;
	int match_result;

	if (iic_use_direct_match(ia, cf, compat_data, &match_result))
		return match_result;
	
	/* indirect config - check typical address */
	if (ia->ia_addr == S390_ADDR)
		return I2C_MATCH_ADDRESS_ONLY;

	return 0;
}

static void
s390rtc_attach(device_t parent, device_t self, void *arg)
{
	struct s390rtc_softc *sc = device_private(self);
	struct i2c_attach_args *ia = arg;
	uint8_t reg[1];

	aprint_naive(": Real-time Clock\n");
	aprint_normal(": Seiko Instruments 35390A Real-time Clock\n");

	sc->sc_tag = ia->ia_tag;
	sc->sc_addr = ia->ia_addr;
	sc->sc_dev = self;

	/* Reset the chip and turn on 24h mode, after power-off or battery. */
	if (s390rtc_read(sc, S390_STATUS1, reg, sizeof(reg)))
		return;
	if (reg[0] & (S390_ST1_POC | S390_ST1_BLD)) {
		reg[0] |= S390_ST1_24H | S390_ST1_RESET;
		if (s390rtc_write(sc, S390_STATUS1, reg, sizeof(reg)))
			return;
	}

	/* Disable the test mode, when enabled. */
	if (s390rtc_read(sc, S390_STATUS2, reg, sizeof(reg)))
		return;
	if ((reg[0] & S390_ST2_TEST)) {
		reg[0] &= ~S390_ST2_TEST;
		if (s390rtc_write(sc, S390_STATUS2, reg, sizeof(reg)))
			return;
	}

	sc->sc_todr.cookie = sc;
	sc->sc_todr.todr_gettime = NULL;
	sc->sc_todr.todr_settime = NULL;
	sc->sc_todr.todr_gettime_ymdhms = s390rtc_gettime_ymdhms;
	sc->sc_todr.todr_settime_ymdhms = s390rtc_settime_ymdhms;
	sc->sc_todr.todr_setwen = NULL;
	todr_attach(&sc->sc_todr);
}

static int
s390rtc_gettime_ymdhms(struct todr_chip_handle *ch, struct clock_ymdhms *dt)
{
	struct s390rtc_softc *sc = ch->cookie;
	uint8_t bcd[S390_RT1_NBYTES];
	int error;

	if ((error = s390rtc_read(sc, S390_REALTIME1, bcd,
				  S390_RT1_NBYTES)) != 0)
		return error;

	/*
	 * Convert the register values into something useable.
	 */
	dt->dt_sec = bcdtobin(bcd[S390_RT1_SECOND]);
	dt->dt_min = bcdtobin(bcd[S390_RT1_MINUTE]);
	dt->dt_hour = bcdtobin(bcd[S390_RT1_HOUR] & 0x3f);
	dt->dt_day = bcdtobin(bcd[S390_RT1_DAY]);
	dt->dt_mon = bcdtobin(bcd[S390_RT1_MONTH]);
	dt->dt_year = bcdtobin(bcd[S390_RT1_YEAR]) + 2000;

	return 0;
}

static int
s390rtc_settime_ymdhms(struct todr_chip_handle *ch, struct clock_ymdhms *dt)
{
	struct s390rtc_softc *sc = ch->cookie;
	uint8_t bcd[S390_RT1_NBYTES];

	/*
	 * Convert our time representation into something the S-xx390
	 * can understand.
	 */
	bcd[S390_RT1_SECOND] = bintobcd(dt->dt_sec);
	bcd[S390_RT1_MINUTE] = bintobcd(dt->dt_min);
	bcd[S390_RT1_HOUR] = bintobcd(dt->dt_hour);
	bcd[S390_RT1_DAY] = bintobcd(dt->dt_day);
	bcd[S390_RT1_WDAY] = bintobcd(dt->dt_wday);
	bcd[S390_RT1_MONTH] = bintobcd(dt->dt_mon);
	bcd[S390_RT1_YEAR] = bintobcd(dt->dt_year % 100);

	return s390rtc_write(sc, S390_REALTIME1, bcd, S390_RT1_NBYTES);
}

static int
s390rtc_read(struct s390rtc_softc *sc, int reg, uint8_t *buf, size_t len)
{
	int i, error;

	if ((error = iic_acquire_bus(sc->sc_tag, 0)) != 0) {
		aprint_error_dev(sc->sc_dev,
		    "%s: failed to acquire I2C bus\n", __func__);
		return error;
	}

	if ((error = iic_exec(sc->sc_tag, I2C_OP_READ_WITH_STOP,
			      sc->sc_addr + reg, NULL, 0, buf, len, 0)) != 0) {
		iic_release_bus(sc->sc_tag, 0);
		aprint_error_dev(sc->sc_dev,
		    "%s: failed to read reg%d\n", __func__, reg);
		return error;
	}

	iic_release_bus(sc->sc_tag, 0);

	/* this chip returns each byte in reverse order */
	for (i = 0; i < len; i++)
		buf[i] = bitreverse(buf[i]);

	return 0;
}

static int
s390rtc_write(struct s390rtc_softc *sc, int reg, uint8_t *buf, size_t len)
{
	int i, error;

	if ((error = iic_acquire_bus(sc->sc_tag, 0)) != 0) {
		aprint_error_dev(sc->sc_dev,
		    "%s: failed to acquire I2C bus\n", __func__);
		return error;
	}

	/* this chip expects each byte in reverse order */
	for (i = 0; i < len; i++)
		buf[i] = bitreverse(buf[i]);

	if ((error = iic_exec(sc->sc_tag, I2C_OP_WRITE_WITH_STOP,
			      sc->sc_addr + reg, NULL, 0, buf, len, 0)) != 0) {
		iic_release_bus(sc->sc_tag, 0);
		aprint_error_dev(sc->sc_dev,
		    "%s: failed to write reg%d\n", __func__, reg);
		return error;
	}

	iic_release_bus(sc->sc_tag, 0);
	return 0;
}

static uint8_t
bitreverse(uint8_t x)
{
	static unsigned char nibbletab[16] = {
		0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15
	};

	return (nibbletab[x & 15] << 4) | nibbletab[x >> 4];
}
