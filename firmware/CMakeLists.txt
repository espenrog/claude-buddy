cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(claude_buddy)

target_sources(app PRIVATE
  src/main.c
  src/ble_svc.c
  src/mood.c
  src/buddy_cfg.c
  src/cfg_store.c
  src/face.c
  src/sound.c
  src/battery.c
  src/battery_gauge.c
  src/battery_log.c
  src/measure_mode.c
  src/battery_log_store.c
  src/buddy_clock.c
  src/buzzer.c
  src/ui.c
  src/motion.c
  src/shake.c
)

target_sources_ifdef(CONFIG_BUDDY_SMP_UPDATE app PRIVATE src/smp_update.c)

# The battery gauge needs a model of the cell. The SDK's sample ships an example; replace it with a model of your own cell
# (made with the nPM PowerUP app) by passing -DBUDDY_BATTERY_MODEL_DIR=<directory containing battery_model.inc>, or by setting
# the environment variable of the same name (sysbuild does not forward custom -D variables to the application; tools/build-xiao.sh uses the environment).
# The model is not copied into this repository because the SDK's files have their own license.
if(CONFIG_BUDDY_FUEL_GAUGE)
  if(NOT DEFINED BUDDY_BATTERY_MODEL_DIR AND DEFINED ENV{BUDDY_BATTERY_MODEL_DIR})
    set(BUDDY_BATTERY_MODEL_DIR $ENV{BUDDY_BATTERY_MODEL_DIR})
  endif()
  if(NOT DEFINED BUDDY_BATTERY_MODEL_DIR)
    set(BUDDY_BATTERY_MODEL_DIR ${ZEPHYR_NRF_MODULE_DIR}/samples/pmic/native/npm13xx_fuel_gauge/src)
  endif()
  target_include_directories(app PRIVATE ${BUDDY_BATTERY_MODEL_DIR})
endif()
