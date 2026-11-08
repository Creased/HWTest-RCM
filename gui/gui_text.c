/*
 * hwtest GUI - what the checks mean, in plain words.
 *
 * Every finding the probes record (dx_set) has a key; this is where each key
 * gets a name a console owner understands and a sentence for when it is not
 * fine: what it means for them and, where there is one, what to try. The
 * technical reason the probe gave stays one tap away, under the plain text.
 *
 * The result categories group the verdict's rows (g_dx_macros) the way an
 * owner thinks about the console - battery, charging, storage, Wi-Fi - and
 * name the report pages behind each one.
 */
#include "gui.h"
#include "gui_text.h"

static const gui_check_text_t s_checks[] = {
    /* Battery */
    { "batt_health", "Battery wear",
      "The battery holds noticeably less charge than when it was new. It "
      "still works, but runs out sooner. A new battery fixes this.",
      NULL, NULL },
    { "batt_ntc", "Battery temperature",
      "The battery is very cold or very warm right now. Let the console "
      "reach room temperature and check again.",
      "The battery temperature reading is impossible. The battery or its "
      "connector may be faulty, and the console may refuse to charge.", NULL },
    { "fuel_devname", "Battery level chip",
      NULL,
      "The chip that measures the battery level does not answer as it "
      "should. The battery percentage may be wrong.", NULL },
    { "xc_charge_dir", "Charge reaching the battery",
      "The charger says it is charging, but the battery is not receiving "
      "current. The battery or the charging circuit may be faulty.",
      NULL, NULL },

    /* Charging */
    { "charger_id", "Charging chip",
      "The charging chip is not the original part, most likely replaced "
      "during a repair.",
      "The charging chip does not respond. The console will most likely "
      "not charge. This needs a repair on the board.", NULL },
    { "charger_pg", "Charger power input",
      NULL,
      "A charger is plugged in, but its power is not being accepted. Try "
      "another charger and cable; if it stays, the charging circuit needs "
      "a repair.", NULL },
    { "charger_fault", "Charging faults",
      NULL,
      "The charging chip has recorded a fault, such as too high a voltage "
      "or overheating. Use an official charger and try again.", NULL },
    { "usb_pd", "USB-C power chip",
      NULL,
      "The USB-C power chip does not respond. Charging and TV output "
      "through the dock may not work. This needs a repair on the board.",
      NULL },
    { "xc_vbus_acok", "USB-C power detection",
      NULL,
      "The chips that watch the USB-C port disagree about whether a "
      "charger is plugged in. The port or one of those chips may be "
      "damaged.", NULL },

    /* Power system */
    { "pmic_rails", "Internal voltages",
      "Some internal voltages are set differently from usual. This is "
      "usually harmless.",
      "Some internal power supplies are outside their safe range. This can "
      "cause crashes or a console that does not start.", NULL },
    { "pmic_nverc", "Last shutdown",
      "The console last shut down because of a battery or power event, "
      "such as an empty battery.",
      "The power chip recorded a serious event: overheating, an "
      "over-current or a watchdog reset. This points to a hardware fault.",
      NULL },
    { "pmic_irqsd", "Power warnings",
      "The power chip flagged a warning since it last powered up.",
      NULL, NULL },
    { "pmic_intlbt", "Low battery or heat warning",
      "The power chip flagged a low battery or a high temperature since it "
      "last powered up.", NULL, NULL },
    { "max77812", "Memory and graphics power chip",
      NULL,
      "A power chip that feeds the memory and graphics does not respond, "
      "or is out of range. The console may not start.", NULL },
    { "max77621_cpu", "Processor power chip",
      NULL,
      "The power chip for the main processor does not respond. The console "
      "cannot start its processor and stays on a black screen.", NULL },
    { "max77621_gpu", "Graphics power chip",
      NULL,
      "The power chip for the graphics processor does not respond. The "
      "home menu and games need it.", NULL },

    /* Temperature and fan */
    { "tmp451_id", "Temperature sensor",
      NULL,
      "The temperature sensor does not respond. The fan cannot be "
      "regulated and the console may overheat.", NULL },
    { "soc_die_temp", "Processor temperature",
      "The processor is warmer than usual for a console doing nothing. "
      "Check that the vents are clear and the fan works.",
      "The processor's temperature sensor is disconnected, so the fan "
      "cannot follow the temperature.", NULL },
    { "pcb_temp", "Board temperature",
      "The inside of the console is warm. Let it cool down; if it stays "
      "hot, the cooling may need cleaning.", NULL, NULL },
    { "fan_stalled", "Fan",
      "The fan seems to spin, but its speed signal is missing. Listen for "
      "it; the speed sensor wire may be damaged.",
      "The fan did not spin when asked to. The console will overheat. The "
      "fan or its connector needs replacing.", NULL },

    /* Storage */
    { "emmc_health", "Internal storage wear",
      "The internal storage is getting worn, or does not report its wear. "
      "Keep a backup of your data.",
      "The internal storage has reached the end of its life. Data loss is "
      "likely: back up now and plan a replacement.", NULL },
    { "emmc_bus", "Internal storage connection",
      NULL,
      "The internal storage is not fully connected. This points to damage "
      "on the board or a failing storage chip.", NULL },
    { "sd_bus", "SD card connection",
      "The SD card runs in a slow, reduced mode. Clean its contacts or try "
      "another card.", NULL, NULL },
    { "wlan_cal", "Wi-Fi factory settings",
      NULL,
      "The Wi-Fi settings stored at the factory are missing or damaged. "
      "Wi-Fi may not work.", NULL },
    { "cal0_hash", "Factory data",
      NULL,
      "The console's factory data is damaged. Online features and some "
      "hardware may not work.", NULL },
    { "cal0_panel", "Original screen",
      "The screen fitted is not the one this console left the factory "
      "with: it has been replaced.", NULL, NULL },
    { "cal0_model", "Original storage",
      "The factory data does not match this console's board: the storage "
      "may come from another console.", NULL, NULL },
    { "prodinfo", "Factory data",
      "The factory data could not be read back correctly.",
      NULL, "no keys",
      "Not read: there is no usable sd:/switch/prod.keys on the SD card. Lockpick_RCM "
      "writes it; with it the serial number and the factory data are "
      "checked too." },
    { "gpt", "Storage layout",
      NULL,
      "The table that describes the internal storage is damaged. The "
      "system will probably not start.", NULL },
    { "xc_emmc_mode", "Storage speed",
      "The internal storage runs at a speed its connection cannot fully "
      "support.", NULL, NULL },
    { "emmc_errors", "Internal storage reads",
      NULL,
      "Reading the internal storage failed during the check. The storage "
      "chip or its connection is failing.", NULL },
    { "sd_errors", "SD card reads",
      "The SD card needed several tries to be read. Try another card.",
      "The SD card could not be read reliably. Try another card.", NULL },
    { "gc_asic", "Game card reader",
      "The game card reader did not answer properly. Game cards may not "
      "be read.",
      "The game card reader could not be powered.", NULL },

    /* Wireless */
    { "bt_hci", "Bluetooth",
      NULL,
      "The Bluetooth radio did not start. Wireless controllers will not "
      "connect.", "inconclusive" },
    { "wifi_pcie", "Wi-Fi",
      "The Wi-Fi chip answers, but not quite as expected.",
      "The Wi-Fi chip did not respond. Wi-Fi will not work.", NULL },

    /* Processor and memory */
    { "cpu_complex", "Main processor",
      NULL,
      "The main processor could not be started. The console will not get "
      "past a black screen.", NULL },
    { "dram_sym", "Memory",
      NULL,
      "The two halves of the memory do not match. This points to a memory "
      "fault. Run the RAM Test.", NULL },
    { "plls", "System clocks",
      NULL,
      "Some internal clocks did not start. The console may be unstable.",
      NULL },

    /* Sound */
    { "audio_codec", "Audio chip",
      "The audio chip did not answer. The speakers and headphones may be "
      "silent.", NULL, NULL },
    { "audio_clocks", "Audio system",
      NULL,
      "The audio part of the processor could not be started.", NULL },
    { "audio_beep", "Sound playback",
      NULL,
      "Sound could not be sent to the audio chip.", NULL },

    /* Screen */
    { "dsi_id", "Screen connection",
      NULL,
      "The screen does not answer. Its cable may be loose, or the screen "
      "may be damaged.", NULL },
    { "backlight", "Backlight",
      "The screen's backlight looks off or very dim.", NULL, NULL },

    /* Touch and sensors */
    { "touch_id", "Touchscreen chip",
      NULL,
      "The touchscreen does not respond. Check the screen's cable; the "
      "touch panel may need replacing.", NULL },
    { "touch_ito", "Touch panel self-test",
      "The touch panel's self-test could not give a result.",
      "The touch panel has a broken line. Parts of the screen will not "
      "respond to touch.", "not run" },
    { "als_id", "Light sensor",
      NULL,
      "The light sensor does not respond. Automatic brightness will not "
      "work.", NULL },

    /* System and security */
    { "soc_pmic_otp", "Power chip model",
      NULL,
      "The main power chip is not the right model for this console, most "
      "likely a wrong replacement part.", NULL },
    { "fuses_pkg1", "System version fuses",
      "The system software is newer than the console's fuses. This is "
      "normal on a modified console; the official system updates them on "
      "its next start.",
      "The console's fuses are newer than its system software. The "
      "official system will not start.", NULL },
    { "kfuse", "Security fuses",
      "The security fuses could not be read in time.",
      "The security fuses are damaged. The official system will not "
      "start.", NULL },
};

const gui_check_text_t *gui_check_text(const char *key)
{
    for (u32 i = 0; i < ARRAY_SIZE(s_checks); i++)
        if (!strcmp(s_checks[i].key, key))
            return &s_checks[i];
    return NULL;
}

/* The readings each category shows, as the probes print them. A reading
 * the run did not produce (another board, a probe that stopped early) is
 * left out. */
#define PWR "Power & charging"

static const gui_fact_t s_f_battery[] = {
    { "Charge",           PWR, "[MAX17050", "SOC",       FACT_VALUE },
    { "Capacity",         PWR, "[MAX17050", "FullCap",   FACT_VALUE },
    { "Design capacity",  PWR, "[MAX17050", "DesignCap", FACT_VALUE },
    { "Charge cycles",    PWR, "[MAX17050", "Cycles",    FACT_VALUE },
    { "Voltage",          PWR, "[MAX17050", "VCELL",     FACT_VALUE },
    { "Current",          PWR, "[MAX17050", "Current",   FACT_VALUE },
    { "Temperature",      PWR, "[MAX17050", "Temp",      FACT_VALUE },
    { NULL },
};

static const gui_fact_t s_f_charging[] = {
    { "Charger",          PWR, "[BQ24193", "CHRG_STAT",   FACT_PAREN },
    { "Power input",      PWR, "[BQ24193", "VBUS_STAT",   FACT_PAREN },
    { "Charger faults",   PWR, "[BQ24193", "FAULT (now",  FACT_PAREN },
    { "Input limit",      PWR, "[BQ24193", "IN current",  FACT_VALUE },
    { "Charge current",   PWR, "[BQ24193", "FastCharge",  FACT_VALUE },
    { "USB-C cable",      PWR, "[BM92T36", "Cable",       FACT_VALUE },
    { NULL },
};

static const gui_fact_t s_f_power[] = {
    { "Processor supply", PWR, "[MAX77620 regulators", "SD0",  FACT_CUT },
    { "Memory supply",    PWR, "[MAX77620 regulators", "SD1",  FACT_CUT },
    { "1.8 V supply",     PWR, "[MAX77620 regulators", "SD3",  FACT_CUT },
    { "Display supply",   PWR, "[MAX77620 regulators", "LDO0", FACT_CUT },
    { "Touch supply",     PWR, "[MAX77620 regulators", "LDO6", FACT_CUT },
    { "CPU/GPU regulator", PWR, "[MAX77812",           "Variant", FACT_CUT },
    { NULL },
};

static const gui_fact_t s_f_thermal[] = {
    { "Processor",        PWR, "[TMP451",       "SoC die temp", FACT_VALUE },
    { "Board",            PWR, "[TMP451",       "PCB temp",     FACT_VALUE },
    { "Battery",          PWR, "[MAX17050",     "Temp",         FACT_VALUE },
    { "Fan speed (rpm)",  PWR, "[Cooling fan",  "Tach RPM",     FACT_CUT },
    { NULL },
};

static const gui_fact_t s_f_storage[] = {
    { "Internal storage", "Storage", "[eMMC partitions",   "USER (GPP)",   FACT_PAREN },
    { "Internal chip",    "Storage", "[eMMC (SDMMC4)",     "prod_name",    FACT_VALUE },
    { "Wear",             "Storage", "[eMMC health",       "Life used A",  FACT_PAREN },
    { "End of life",      "Storage", "[eMMC health",       "PRE_EOL_INFO", FACT_PAREN },
    { "SD card",          "Storage", "[SD card (SDMMC1)",  "size",         FACT_VALUE },
    { "SD card speed",    "Storage", "[SD card (SDMMC1)",  "init mode",    FACT_CUT },
    { "Game card slot",   "Storage", "[Gamecard",          "Card detect",  FACT_VALUE },
    { NULL },
};

static const gui_fact_t s_f_wireless[] = {
    { "Bluetooth",        "Wireless", "[Bluetooth radio",     "Result",       FACT_VALUE },
    { "Bluetooth chip",   "Wireless", "[Bluetooth radio",     "Manufacturer", FACT_PAREN },
    { "Wi-Fi",            "Wireless", "[Wi-Fi radio - PCIe",  "Result",       FACT_VALUE },
    { "Wi-Fi chip",       "Wireless", "[Wi-Fi radio - PCIe",  "EP config",    FACT_VALUE },
    { "Wi-Fi link",       "Wireless", "[Wi-Fi radio - PCIe",  "RP1 link",     FACT_CUT },
    { NULL },
};

static const gui_fact_t s_f_cpu[] = {
    { "Processor",        "SoC",             NULL,             "Chip ID",     FACT_PAREN },
    { "Revision",         "SoC",             NULL,             "Major.Minor", FACT_VALUE },
    { "CPU start-up",     "Wireless",        "[Wi-Fi radio - PCIe", "CPU0",   FACT_CUT },
    { "Memory",           "Memory & clocks", "[DRAM identity", "Type",        FACT_VALUE },
    { "Memory maker",     "Memory & clocks", "[DRAM identity", "Chip 0 vend", FACT_VALUE },
    { "Memory chip 1",    "Memory & clocks", "[DRAM identity", "Chip 0 dens", FACT_VALUE },
    { "Memory chip 2",    "Memory & clocks", "[DRAM identity", "Chip 1 dens", FACT_VALUE },
    { NULL },
};

static const gui_fact_t s_f_sound[] = {
    { "Audio chip",       "Audio", "[ALC5639",    "Result", FACT_VALUE },
    { "Speakers and jack", "Audio", "[Audio test", "Result", FACT_VALUE },
    { NULL },
};

static const gui_fact_t s_f_screen[] = {
    { "Panel",            "Display", "[DSI panel ID",       "Model",      FACT_VALUE },
    { "Panel ID",         "Display", "[DSI panel ID",       "Decoded ID", FACT_VALUE },
    { "Backlight",        "Display", "[Display backlight",  "PWM duty",   FACT_PAREN },
    { NULL },
};

static const gui_fact_t s_f_touch[] = {
    { "Touch panel",      "Inputs", "[Touch panel",    "vendor",    FACT_CUT },
    { "Touch firmware",   "Inputs", "[Touch panel",    "fw_id",     FACT_VALUE },
    { "Touch self-test",  "Inputs", "[Touch panel",    "ITO test",  FACT_VALUE },
    { "Light (lux)",      "Inputs", "[BH1730",         "Lux",       FACT_VALUE },
    { "Left rail",        "Inputs", "[Joy-Con rail",   "JC-L rail", FACT_CUT },
    { "Right rail",       "Inputs", "[Joy-Con rail",   "JC-R rail", FACT_CUT },
    { NULL },
};

static const gui_fact_t s_f_system[] = {
    { "Serial number",    "Storage", "[Switch serial",   "Serial",      FACT_VALUE },
    { "Fuses burnt",      "Fuses",   "[Fuses - identity", "Burnt fuses", FACT_CUT },
    { "RCM",              "Fuses",   "[Fuses - identity", "Patched RCM", FACT_VALUE },
    { "AutoRCM",          "Storage", "[AutoRCM",         "AutoRCM",     FACT_VALUE },
    { "hekate",           "Storage", "[SD card content", "hekate",      FACT_CUT },
    { "Atmosphere",       "Storage", "[SD card content", "atmosphere",  FACT_CUT },
    { NULL },
};

const gui_cat_t g_gui_cats[] = {
    { "Battery", ICON_BATTERY, 0x34C759,
      { "Battery" }, { "Power & charging" },
      "Holds its charge normally.", s_f_battery },
    { "Charging", ICON_PLUG, 0xFFB020,
      { "Charger" }, { "Power & charging" },
      "Charging and USB-C power work.", s_f_charging },
    { "Power system", ICON_BOLT, 0xFFD60A,
      { "PMIC" }, { "Power & charging", "Raw state" },
      "Every internal power supply is normal.", s_f_power },
    { "Temperature and fan", ICON_THERMO, 0xFF7A59,
      { "Thermal" }, { "Power & charging" },
      "Temperatures are normal and the fan spins.", s_f_thermal },
    { "Storage", ICON_STORAGE, 0x5AC8FA,
      { "Storage" }, { "Storage" },
      "Internal storage and SD card are healthy.", s_f_storage },
    { "Wi-Fi and Bluetooth", ICON_WIFI, 0x3D8BFF,
      { "Wireless" }, { "Wireless" },
      "Both radios start and answer.", s_f_wireless },
    { "Processor and memory", ICON_CHIP, 0xB48CFF,
      { "CPU complex", "Memory" }, { "Memory & clocks", "Wireless" },
      "The processor starts and the memory is consistent.", s_f_cpu },
    { "Sound", ICON_SPEAKER, 0xFF6FB5,
      { "Audio" }, { "Audio" },
      "The audio chip and sound path work.", s_f_sound },
    { "Screen", ICON_SCREEN, 0x38D6E0,
      { "Display" }, { "Display" },
      "The screen and its backlight answer.", s_f_screen },
    { "Touch and sensors", ICON_TOUCH, 0x7CC4FF,
      { "Inputs" }, { "Inputs" },
      "The touchscreen and light sensor answer.", s_f_touch },
    { "System and security", ICON_SHIELD, 0x9AA3B5,
      { "Boot integrity", "Identity" }, { "SoC", "Fuses", "Storage" },
      "Fuses and system data are consistent.", s_f_system },
};
const u32 g_gui_cat_n = ARRAY_SIZE(g_gui_cats);
