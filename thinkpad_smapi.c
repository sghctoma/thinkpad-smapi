#include <sys/cdefs.h>

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/stat.h>
#include <machine/bus.h>
#include <machine/resource.h>
#include <sys/rman.h>
#include <isa/rtc.h>
#include <machine/pc/bios.h>

#include "thinkpad_smapi.h"

static devclass_t thinkpad_smapi_devclass;

static struct cdevsw thinkpad_smapi_cdevsw = {
	.d_version =	D_VERSION,
	.d_name =	"thinkpad_smapi",
};

/* Checks whether we have a suitable SMAPI */
static int
check_thinkpad_smapi(void)
{
	u_int16_t id;

	id = rtcin(0x7c) | rtcin(0x7d) << 8;
	if (id != THINKPAD_SMAPI_ID) {
		return(ENXIO);
	}

	return(0);
}

/*
 * Gets APM Control Port from CMOS.
 * XXX: Probably unnecessery, because APM control port seems
 * to be 0xb2 on all ThinkPads.
 */
static u_int16_t
get_apm_control_port(void)
{
	u_int16_t port;

	port = rtcin(0x7E) | rtcin(0x7F) << 8;
	return(port);
}

/*
 * SMAPI functions
 */

/*
 * Converts SMAPI error messages to ERRNO values, and
 * returns the error message in msg.
 */
static int
smapi_error(u_int8_t retcode, const char **msg)
{
	switch(retcode) {
	case 0x00:
		if (msg != NULL) *msg = smapi_messages[0];
		return(0);
	case 0x53:
		if (msg != NULL) *msg = smapi_messages[1];
		return(ENXIO);
	case 0x81:
		if (msg != NULL) *msg = smapi_messages[2];
		return(EINVAL);
	case 0x86:
		if (msg != NULL) *msg = smapi_messages[3];
		return(ENOTSUP);
	case 0x90:
		if (msg != NULL) *msg = smapi_messages[4];
		return(EIO);
	case 0x91:
		if (msg != NULL) *msg = smapi_messages[5];
		return(EIO);
	case 0x92:
		if (msg != NULL) *msg = smapi_messages[6];
		return(EBUSY);
	case 0xa0:
		if (msg != NULL) *msg = smapi_messages[7];
		return(EIO);
	case 0xa1:
		if (msg != NULL) *msg = smapi_messages[8];
		return(EBUSY);
	case 0xa2:
		if (msg != NULL) *msg = smapi_messages[9];
		return(ENXIO);
	case 0xa3:
		if (msg != NULL) *msg = smapi_messages[10];
		return(EIO);
	case 0xa4:
		if (msg != NULL) *msg = smapi_messages[11];
		return(EINVAL);
	case 0xa5:
		if (msg != NULL) *msg = smapi_messages[12];
		return(EINVAL);
	}

	return(EIO);
}

/* Calls SMAPI. */
static int
smapi_call(struct thinkpad_smapi_softc *sc,
    u_int32_t iebx, u_int32_t iecx, u_int32_t iedi, u_int32_t iesi,
    u_int32_t *oebx, u_int32_t *oecx, u_int32_t *oedx, u_int32_t *oedi,
    u_int32_t *oesi, const char **msg)
{
	u_int32_t teax, tebx, tecx, tedx, tedi, tesi;
	u_int8_t rc;
	int i, ret;

	for (i = 0; i < SMAPI_CALL_RETRIES; ++i) {

		__asm__ __volatile__ (
		    "out %%al, %[apm_control_port]\n\t"
		    "out %%al, $0x4f"
		    : "=a" (teax),
		      "=b" (tebx),
		      "=c" (tecx),
		      "=d" (tedx),
		      "=D" (tedi),
		      "=S" (tesi)
		    : [apm_control_port] "d" (sc->apm_control_port),
		      "a" (THINKPAD_SMAPI_EAX),
		      "b" (iebx),
		      "c" (iecx),
		      "D" (iedi),
		      "S" (iesi)
		    : "cc");
		    //: "%eax", "%ebx", "%ecx", "%edx", "%edi", "%esi");

		DELAY(50);

		if (oebx != NULL) *oebx = tebx;
		if (oecx != NULL) *oecx = tecx;
		if (oedx != NULL) *oedx = tedx;
		if (oedi != NULL) *oedi = tedi;
		if (oesi != NULL) *oesi = tesi;

		rc = (teax >> 8) & 0xff;
		ret = smapi_error(rc, msg);

		if (ret != EBUSY)
			return(ret);
	}

	return(ret);
}

/*
 * Wrapper around smapi_call to get battery
 * properties through SMAPI.
 */
static int
smapi_battery_read(struct thinkpad_smapi_softc *sc,
    u_int32_t function, u_int32_t input, u_int32_t *oecx,
    u_int32_t *oedi, u_int32_t *oesi, const char **msg)
{

	return smapi_call(sc, function, input, 0, 0, NULL, oecx, NULL,
	    oedi, oesi, msg);
}

/*
 * Wrapper around smapi_call to set battery
 * properties through SMAPI.
 */
static int
smapi_battery_write(struct thinkpad_smapi_softc *sc,
    u_int32_t function, u_int32_t input, u_int32_t iedi,
    u_int32_t iesi, const char **msg)
{

	return smapi_call(sc, function + 1, input, iedi, iesi,
	    NULL, NULL, NULL, NULL, NULL, msg);
}

/*
 * Gets start/stop charge threshold. The battery will be charged
 * only if the charge drops below start threshold, and only until
 * the charge reaches stop threshold.
 */
static int
get_threshold(struct thinkpad_smapi_softc *sc,enum BATTERY bat,
    enum THRESHOLD type, u_int32_t *thresh, u_int32_t *oedi, u_int32_t *oesi)
{
	const char *msg;
	u_int32_t function, ecx;
	int ret;

	function = (type == START) ? THINKPAD_SMAPI_START_THRESHOLD :
	    THINKPAD_SMAPI_STOP_THRESHOLD;
	ecx = (bat + 1) << 8;
	ret = smapi_battery_read(sc, function, ecx, &ecx, oedi, oesi, &msg);
	if (ret) {
		device_printf(sc->dev,
		    "could not get %s threshold for %s: %s\n",
		    THRESHOLD_STRING(type), BATTERY_STRING(bat), msg);
		return(ret);
	}

	if ((ecx & 0x00000100) == 0) {
		device_printf(sc->dev,
		    "could not get %s threshold for %s: %s\n",
		    THRESHOLD_STRING(type), BATTERY_STRING(bat), msg);
		return(EIO);
	}

	if (thresh != NULL)
		*thresh = ecx & 0xff;

	return(0);
}

/*
 * Sets start/stop charge threshold. See the comment of
 * get_threshold!
 */
static int
set_threshold(struct thinkpad_smapi_softc *sc, enum BATTERY bat,
    enum THRESHOLD type, u_int32_t thresh)
{
	const char *msg;
	u_int32_t function;
	u_int32_t ecx, edi, esi;
	int ret;

	function = (type == START) ? THINKPAD_SMAPI_START_THRESHOLD :
	    THINKPAD_SMAPI_STOP_THRESHOLD;
	ret = get_threshold(sc, bat, type, NULL, &edi, &esi);
	if (ret)
		return(ret);

	ecx = (bat + 1) << 8 | thresh;
	ret = smapi_battery_write(sc, function, ecx, edi, esi, &msg);
	if (ret)
		device_printf(sc->dev,
		    "set %s threshold to %d for %s failed: %s\n",
		    THRESHOLD_STRING(type), thresh, BATTERY_STRING(bat), msg);
	else
		device_printf(sc->dev, "new %s threshold for %s: %d\n",
		    THRESHOLD_STRING(type), BATTERY_STRING(bat), thresh);
	return(0);
}

/*
 * Gets inhibit charge duration in minutes. The battery
 * is forbidden to charge for given minutes.
 */
static int
get_inhibit_charge_minutes(struct thinkpad_smapi_softc *sc,
    enum BATTERY bat, u_int32_t *minutes, u_int32_t *oecx)
{
	u_int32_t ecx, esi;
	const char *msg;
	int ret;

	ecx = (bat + 1) << 8;
	ret = smapi_battery_read(sc, THINKPAD_SMAPI_INHIBIT_CHARGE, ecx, &ecx,
	    NULL, &esi, &msg);
	if (ret) {
		device_printf(sc->dev,
		    "could not get inhibit charge minutes for %s: %s\n",
		    BATTERY_STRING(bat), msg);
		return(ret);
	}
	if ((ecx & 0x0100) == 0) {
		device_printf(sc->dev,
		    "incorrect value in ECX (0x%0x) for %s\n",
		    ecx, BATTERY_STRING(bat));
		return(EIO);
	}
	if (minutes != NULL)
		*minutes = (ecx & 0x0001) == 0 ? 0 : esi;
	if (oecx != NULL)
		*oecx = ecx;

	return(0);
}

/*
 * Set inhibit charge duration. The battery will not be
 * charged for given minutes.
 */
static int
set_inhibit_charge_minutes(struct thinkpad_smapi_softc *sc,
    enum BATTERY bat, u_int32_t minutes)
{
	u_int32_t ecx;
	const char *msg;
	int ret;

	ret = get_inhibit_charge_minutes(sc, bat, NULL, &ecx);
	if (ret)
		return ret;

	ecx = (bat + 1) << 8 | (ecx & 0x00fe) | (minutes > 0 ? 0x0001 : 0x0000);
	if (minutes > 0xffff)
		minutes = 0xffff;
	ret = smapi_battery_write(sc, THINKPAD_SMAPI_INHIBIT_CHARGE, ecx, 0,
	    minutes, &msg);
	if (ret)
		device_printf(sc->dev,
		    "set inhibit charge minutes to %d for %s failed: %s\n",
		    minutes, BATTERY_STRING(bat), msg);
	else
		device_printf(sc->dev,
		    "new inhibit charge minutes for %s: %d\n",
		    BATTERY_STRING(bat), minutes);
	return(ret);
}

/* Gets whether force discharge is enabled or disabled. */
static int
get_force_discharge(struct thinkpad_smapi_softc *sc,
    enum BATTERY bat, u_int32_t *enabled)
{
	u_int32_t ecx;
	const char *msg;
	int ret;

	ecx = (bat + 1) << 8;
	ret = smapi_battery_read(sc, THINKPAD_SMAPI_FORCE_DISCHARGE, ecx, &ecx,
	    NULL, NULL, &msg);
	if (ret) {
		device_printf(sc->dev, "could not get fore discharge for %s: "
		    "%s\n", BATTERY_STRING(bat), msg);
		return(ret);
	}
	*enabled = (!(ecx & 0x00000100) && (ecx & 0x00000001));

	return(0);
}

/* Enable/disable forced discharge */
static int
set_force_discharge(struct thinkpad_smapi_softc *sc,
    enum BATTERY bat, u_int32_t enabled)
{
	u_int32_t ecx;
	const char *msg;
	int ret;

	ret = get_force_discharge(sc, bat, &ecx);
	if (ret)
		return ret;

	if ((ecx & 0x00000100) != 0) {
		device_printf(sc->dev, "cannot force discharge for %s\n",
		    BATTERY_STRING(bat));
		return(EIO);
	}

	ecx = (bat + 1) << 8 | (ecx & 0x000000fa) | (enabled ? 0x00000001 : 0);
	ret = smapi_battery_write(sc, THINKPAD_SMAPI_FORCE_DISCHARGE, ecx, 0, 0,
	    &msg);
	if (ret)
		device_printf(sc->dev, "%s force discharge for %s failed: %s\n",
		    (enabled ? "enable" : "disable"), BATTERY_STRING(bat), msg);
	else
		device_printf(sc->dev, "force discharge is %s for %s\n",
		    (enabled ? "enabled" : "disabled"), BATTERY_STRING(bat));
	return(ret);

}

/*
 * Sysctl functions
 */

static int
sysctl_proc_inhibit_charge_minutes(SYSCTL_HANDLER_ARGS)
{
	int error;
	u_int32_t newval, oldval;
	struct thinkpad_smapi_softc *sc;

	sc = arg1;
	get_inhibit_charge_minutes(sc, arg2, &oldval, NULL);
	newval = oldval;
	if ((error = sysctl_handle_int(oidp, &newval, 0, req)) != 0)
		return(error);
	if (newval == oldval)
		return(0);
	if (newval > 65535)
		return(EINVAL);
	if ((error = set_inhibit_charge_minutes(sc, arg2, newval)) != 0)
		return(error);

	return(0);
}

static int
sysctl_proc_start_threshold(SYSCTL_HANDLER_ARGS)
{
	int error;
	u_int32_t newval, oldval, stop;
	struct thinkpad_smapi_softc *sc;

	sc = arg1;
	get_threshold(sc, arg2, STOP, &stop, NULL, NULL);
	get_threshold(sc, arg2, START, &oldval, NULL, NULL);
	newval = oldval;
	if ((error = sysctl_handle_int(oidp, &newval, 0, req)) != 0)
		return(error);
	if (newval == oldval)
		return(0);
	if (newval > stop - 4)
		return(EINVAL);
	if ((error = set_threshold(sc, arg2, START, newval)) != 0)
		return(error);

	return(0);
}

static int
sysctl_proc_force_discharge(SYSCTL_HANDLER_ARGS)
{
	int error;
	u_int32_t newval, oldval;
	struct thinkpad_smapi_softc *sc;

	sc = arg1;
	get_force_discharge(sc, arg2, &oldval);
	newval = oldval;
	if ((error = sysctl_handle_int(oidp, &newval, 0, req)) != 0)
		return(error);
	if (newval == oldval)
		return(0);
	if (!(newval == 0 || newval == 1))
		return(EINVAL);
	if ((error = set_force_discharge(sc, arg2, newval)) != 0)
		return(error);

	return(0);
}

static int
sysctl_proc_stop_threshold(SYSCTL_HANDLER_ARGS)
{
	int error;
	u_int32_t newval, oldval, start;
	struct thinkpad_smapi_softc *sc;

	sc = arg1;
	get_threshold(sc, arg2, START, &start, NULL, NULL);
	get_threshold(sc, arg2, STOP, &oldval, NULL, NULL);
	newval = oldval;
	if ((error = sysctl_handle_int(oidp, &newval, 0, req)) != 0)
		return(error);
	if (newval == oldval)
		return(0);
	if (newval < start + 4 || newval > 100)
		return(EINVAL);
	if ((error = set_threshold(sc, arg2, STOP, newval)) != 0)
		return(error);

	return(0);
}

/* Creates ThinkPad SMAPI sysctl tree */
static void
thinkpad_smapi_sysctl_nodes(enum BATTERY bat, struct thinkpad_smapi_softc *sc)
{
	char name[5];
	struct sysctl_oid *sysctl_tree_bat;

	(bat == BAT0) ? sprintf(name, "bat0") : sprintf(name, "bat1");

	sysctl_tree_bat = SYSCTL_ADD_NODE(&sc->sysctl_ctx,
	    SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO, name,
	    CTLFLAG_RW, 0, "IBM/Lenovo System Management API BAT0");

	/* hw.thinkpad_smapi.batX.start_threshold */
	SYSCTL_ADD_PROC(&sc->sysctl_ctx, SYSCTL_CHILDREN(sysctl_tree_bat),
	   OID_AUTO, "start_threshold", CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_ANYBODY,
	   sc, bat, sysctl_proc_start_threshold, "I", "Start charge threshold");

	/* hw.thinkpad_smapi.batX.stop_threshold */
	SYSCTL_ADD_PROC(&sc->sysctl_ctx, SYSCTL_CHILDREN(sysctl_tree_bat),
	   OID_AUTO, "stop_threshold", CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_ANYBODY,
	   sc, bat, sysctl_proc_stop_threshold, "I", "Stop charge threshold");

	/* hw.thinkpad_smapi.batX.inhibit_charge_minutes */
	SYSCTL_ADD_PROC(&sc->sysctl_ctx, SYSCTL_CHILDREN(sysctl_tree_bat),
	   OID_AUTO, "inhibit_charge_minutes",
	   CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_ANYBODY,
	   sc, bat, sysctl_proc_inhibit_charge_minutes, "I",
	   "Inhibit charge for given minutes");

	/* hw.thinkpad_smapi.batX.force_discharge */
	SYSCTL_ADD_PROC(&sc->sysctl_ctx, SYSCTL_CHILDREN(sysctl_tree_bat),
	   OID_AUTO, "force_discharge", CTLTYPE_UINT|CTLFLAG_RW|CTLFLAG_ANYBODY,
	   sc, bat, sysctl_proc_force_discharge, "I",
	   "Enable/disable force discharge");
}

static void
thinkpad_smapi_sysctl_tree(struct thinkpad_smapi_softc *sc)
{

	/* hw.thinkpad_smapi */
	sysctl_ctx_init(&sc->sysctl_ctx);
	sc->sysctl_tree = SYSCTL_ADD_NODE(&sc->sysctl_ctx,
	    SYSCTL_STATIC_CHILDREN(_hw), OID_AUTO, "thinkpad_smapi",
	    CTLFLAG_RW, 0, "IBM/Lenovo System Management API");

	thinkpad_smapi_sysctl_nodes(BAT0, sc);
	thinkpad_smapi_sysctl_nodes(BAT1, sc);
}

/*
 * Device methods
 */

static void
thinkpad_smapi_identify(driver_t *driver, device_t parent)
{
	device_t child;
	u_int16_t port;

	if (device_find_child(parent, "thinkpad_smapi", -1) == NULL) {
		child = BUS_ADD_CHILD(parent, 0, "thinkpad_smapi", -1);
		port = get_apm_control_port();
		bus_set_resource(child, SYS_RES_IOPORT, 0, port, 1);
		bus_set_resource(child, SYS_RES_IOPORT, 1, SMAPI_PORT, 1);
	}
}

static int
thinkpad_smapi_probe(device_t dev)
{
	if (device_get_unit(dev) != 0)
		return(ENXIO);
	if (check_thinkpad_smapi() != 0)
		return(ENXIO);
	if (bus_get_resource_start(dev, SYS_RES_IOPORT, 0) == 0)
		return(ENXIO);
	if (bus_get_resource_start(dev, SYS_RES_IOPORT, 1) == 0)
		return(ENXIO);
	return (0);
}

static int
thinkpad_smapi_attach(device_t dev)
{
	struct thinkpad_smapi_softc *sc;
	u_int16_t port;

	sc = (struct thinkpad_smapi_softc *) device_get_softc(dev);
	mtx_init(&sc->thinkpad_smapi_mtx, "ThinkPad SMAPI mutex", 0, MTX_DEF);
	port = get_apm_control_port();

	sc->rid1 = 0;
	sc->res1 = bus_alloc_resource(dev, SYS_RES_IOPORT, &sc->rid1,
	    port, port, 1, RF_ACTIVE);
	if (sc->res1 == NULL)
		return (ENXIO);

	sc->rid2 = 1;
	sc->res2 = bus_alloc_resource(dev, SYS_RES_IOPORT, &sc->rid2,
	    SMAPI_PORT, SMAPI_PORT, 1, RF_ACTIVE);
	if (sc->res2 == NULL)
		return(ENXIO);

	sc->dev = dev;
	sc->bat0 = make_dev(&thinkpad_smapi_cdevsw, 0, UID_ROOT, GID_WHEEL,
	    S_IRWXU | S_IRGRP | S_IROTH, "tpsmapi/bat0");
	sc->bat1 = make_dev(&thinkpad_smapi_cdevsw, 1, UID_ROOT, GID_WHEEL,
	    S_IRWXU | S_IRGRP | S_IROTH, "tpsmapi/bat1");
	sc->apm_control_port = port;
	thinkpad_smapi_sysctl_tree(sc);
	return(0);
}

static int
thinkpad_smapi_detach(device_t dev)
{
	struct thinkpad_smapi_softc *sc;

	sc = (struct thinkpad_smapi_softc *)device_get_softc(dev);
	if (sc->res1 != NULL) {
		device_printf(dev, "releasing res1\n");
		bus_release_resource(dev, SYS_RES_IOPORT, sc->rid1, sc->res1);
	}
	if (sc->res2 != NULL) {
		device_printf(dev, "releasing res1\n");
		bus_release_resource(dev, SYS_RES_IOPORT, sc->rid2, sc->res2);
	}
	sysctl_ctx_free(&sc->sysctl_ctx);
	destroy_dev(sc->bat0);
	destroy_dev(sc->bat1);
	mtx_destroy(&sc->thinkpad_smapi_mtx);

	return 0;
}

static int
thinkpad_smapi_modevent (module_t mod, int what, void *arg)
{
	device_t *devices;
	int count;
	int i;

	switch(what) {
	case MOD_LOAD:
		break;
	case MOD_UNLOAD:
		devclass_get_devices(thinkpad_smapi_devclass, &devices, &count);
		for (i = 0; i < count; ++i) {
			device_delete_child(device_get_parent(devices[i]),
			    devices[i]);
		}
		break;
	default:
		break;
	}

	return(0);
}

static device_method_t thinkpad_smapi_methods[] = {
	DEVMETHOD(device_identify,	thinkpad_smapi_identify),
	DEVMETHOD(device_probe,		thinkpad_smapi_probe),
	DEVMETHOD(device_attach,	thinkpad_smapi_attach),
	DEVMETHOD(device_detach,	thinkpad_smapi_detach),

	{ 0, 0 }
};

static driver_t thinkpad_smapi_driver = {
	"thinkpad_smapi",
	thinkpad_smapi_methods,
	sizeof(struct thinkpad_smapi_softc),
};

DRIVER_MODULE(thinkpad_smapi, acpi, thinkpad_smapi_driver,
    thinkpad_smapi_devclass, thinkpad_smapi_modevent, 0);

