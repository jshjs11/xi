# Copyright 2013 Cheolmin Jo (webos21@gmail.com)
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

######################################################
#                          XI                        #
#----------------------------------------------------#
# File    : Makefile                                 #
# Version : 0.0.1                                    #
# Desc    : [xi.src] Makefile.                       #
#----------------------------------------------------#
# History)                                           #
#   - 2008/06/12 : Created by cmjo                   #
######################################################

# PREPARE : set base
basedir = ..

# PREPARE : Check Environment
ifeq ($(TARGET),)
need_warning = "Warning : you are here without proper command!!!!"
include $(basedir)/buildx/antmk/project.mk
include $(basedir)/buildx/antmk/java.mk
include $(basedir)/buildx/antmk/build_$(project_def_target).mk
TARGET = $(project_def_target)
else
need_warning = ""
include $(basedir)/buildx/antmk/project.mk
include $(basedir)/buildx/antmk/java.mk
include $(basedir)/buildx/antmk/build_$(TARGET).mk
endif

# PREPARE : get current directory
xi_src_abs   = $(CURDIR)
xi_src_rel   = $(notdir $(xi_src_abs))

# PREPARE : set target directory
xi_src_target = $(basedir)/amk/$(build_cfg_target)/xi


###################
# build-targets
###################

all: prepare do_build

prepare:
	@echo $(need_warning)
	@$(MKDIR) -p "$(xi_src_target)"
	@echo "================================================================"
	@echo "TARGET                  : $(TARGET)"
	@echo "MAKE_MODE               : $(MAKE_MODE)"
	@echo "----------------------------------------------------------------"
	@echo "xi_src_abs              : $(xi_src_abs)"
	@echo "xi_src_rel              : $(xi_src_rel)"
	@echo "----------------------------------------------------------------"
	@echo "xi_src_target           : $(xi_src_target)"	
	@echo "================================================================"


do_build:
	$(TEST_VAR) "$(OS)" $(TEST_NEQ) "Windows_NT" $(TEST_THEN) \
		$(MAKE) -C base TARGET=$(TARGET) \
	$(TEST_END)
	$(TEST_VAR) "$(OS)" $(TEST_EQ) "Windows_NT" $(TEST_THEN) \
		cmd /c "..\buildx\antmk\build_window.bat $(TARGET) $(MAKE) -C base -f Makefile.win.mk \"TARGET=$(TARGET)\"" \
	$(TEST_END)


clean: prepare
	$(RM) -rf "$(xi_src_target)"

