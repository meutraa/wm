# paths
PREFIX = /usr/local

# Default compile flags (overridable by environment)
CFLAGS ?= -O3 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function -Wno-unused-variable -Wdeclaration-after-statement

# Uncomment to build XWayland support
#CFLAGS += -DXWAYLAND
