#*****************************************************************************
#
# DESCRIPTION: Script for build tool cmake on both unix and windows
#
#*****************************************************************************
#
# Copyright 2003-2023 by Wilson Snyder. This program is free software; you
# can redistribute it and/or modify it under the terms of either the GNU
# Lesser General Public License Version 3 or the Perl Artistic License
# Version 2.0.
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
#
#****************************************************************************/


configure_file(verilated.mk.in ${CMAKE_CURRENT_SOURCE_DIR}/verilated.mk @ONLY)
configure_file(vlpoplar/verilated.mk.in ${CMAKE_CURRENT_SOURCE_DIR}/vlpoplar/verilated.mk @ONLY)
