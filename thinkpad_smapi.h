#include <sys/sysctl.h>

#define SMAPI_CALL_RETRIES	10
#define SMAPI_RETCODE_EOF 	0xff
#define SMAPI_PORT		0x4f
#define SMAPI_PORT_S		"$0x4f"
#define THINKPAD_SMAPI_ID	0x5349
#define THINKPAD_SMAPI_EAX	0x5380

#define THRESHOLD_STRING(t) (t == START ? "start" : "stop")
#define BATTERY_STRING(b) (b == BAT0? "BAT0" : "BAT1")

enum {
	THINKPAD_SMAPI_INHIBIT_CHARGE =  0x2114,
	THINKPAD_SMAPI_START_THRESHOLD = 0x2116,
	THINKPAD_SMAPI_FORCE_DISCHARGE = 0x2118,
	THINKPAD_SMAPI_STOP_THRESHOLD =  0x211a,
};

enum THRESHOLD {
	START,
	STOP,
};

enum BATTERY {
	BAT0,
	BAT1,
};

static const char *smapi_messages[] =
{
	"No error",
	"SMAPI fuction is not available",
	"Invalid parameter",
	"Function is not supported",
	"System error",
	"System is invalid",
	"System is busy",
	"Device error (disk read error)",
	"Device is busy",
	"Device is not attached",
	"Device is disbled",
	"Request parameter is out of range",
	"Request parameter is not accepted",
};

struct thinkpad_smapi_softc {
	struct sysctl_ctx_list	sysctl_ctx;
	struct sysctl_oid	*sysctl_tree;
	struct mtx		thinkpad_smapi_mtx;
	device_t		dev;
	struct cdev		*bat0;
	struct cdev		*bat1;
	u_int16_t		apm_control_port;
	bus_space_tag_t		iot;
	bus_space_handle_t	bsh1;
	bus_space_handle_t	bsh2;
	int			rid1;
	int			rid2;
	struct resource		*res1;
	struct resource		*res2;
};

