#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_ROOT := $(shell dirname $(realpath $(firstword $(MAKEFILE_LIST))))

PROJECT_NAME := yaws-sensor-mcp9808
EXTRA_COMPONENT_DIRS += $(PROJECT_ROOT)/../components
include $(IDF_PATH)/make/project.mk
