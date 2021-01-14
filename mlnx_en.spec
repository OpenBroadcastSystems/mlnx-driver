#
# Copyright (c) 2012 Mellanox Technologies. All rights reserved.
#
# This Software is licensed under one of the following licenses:
#
# 1) under the terms of the "Common Public License 1.0" a copy of which is
#    available from the Open Source Initiative, see
#    http://www.opensource.org/licenses/cpl.php.
#
# 2) under the terms of the "The BSD License" a copy of which is
#    available from the Open Source Initiative, see
#    http://www.opensource.org/licenses/bsd-license.php.
#
# 3) under the terms of the "GNU General Public License (GPL) Version 2" a
#    copy of which is available from the Open Source Initiative, see
#    http://www.opensource.org/licenses/gpl-license.php.
#
# Licensee has the right to choose one of the above licenses.
#
# Redistributions of source code must retain the above copyright
# notice and one of the license notices.
#
# Redistributions in binary form must reproduce both the above copyright
# notice, one of the license notices in the documentation
# and/or other materials provided with the distribution.
#
#

# KMP is disabled by default
%{!?KMP: %global KMP 0}

%global WITH_SYSTEMD %(if ( test -d "%{_unitdir}" > /dev/null); then echo -n '1'; else echo -n '0'; fi)
%global WINDRIVER %(if (grep -qiE "Wind River" /etc/issue /etc/*release* 2>/dev/null); then echo -n '1'; else echo -n '0'; fi)
%global POWERKVM %(if (grep -qiE "powerkvm" /etc/issue /etc/*release* 2>/dev/null); then echo -n '1'; else echo -n '0'; fi)
%global BLUENIX %(if (grep -qiE "Bluenix" /etc/issue /etc/*release* 2>/dev/null); then echo -n '1'; else echo -n '0'; fi)
%global XENSERVER65 %(if (grep -qiE "XenServer.*6\.5" /etc/issue /etc/*release* 2>/dev/null); then echo -n '1'; else echo -n '0'; fi)
# Force python3 on RHEL8, fedora3x, SLES15 and similar:
%global RHEL8 0%{?rhel} >= 8
%global FEDORA3X 0%{?fedora} >= 30
%global SLES15 0%{?suse_version} >= 1500
%global PYTHON3 %{RHEL8} || %{FEDORA3X} || %{SLES15}

# Workaround: To be removed when mlnx_tune has python3 support:
# mlnx_tune is a python2 script. Avoid generating dependencies
# from it in some distributions to avoid dragging in a python2
# dependency
%if %{PYTHON3}
%global __requires_exclude_from mlnx_tune
%endif

%global IS_RHEL_VENDOR "%{_vendor}" == "redhat" || ("%{_vendor}" == "bclinux") || ("%{_vendor}" == "openEuler")

%{!?MEMTRACK: %global MEMTRACK 0}
%{!?MLX4: %global MLX4 1}
%{!?MLX5: %global MLX5 1}
%{!?MLXFW: %global MLXFW 1}

%{!?KVERSION: %global KVERSION %(uname -r)}
%global kernel_version %{KVERSION}
%global krelver %(echo -n %{KVERSION} | sed -e 's/-/_/g')
# take path to kernel sources if provided, otherwise look in default location (for non KMP rpms).
%{!?KSRC: %global KSRC /lib/modules/%{KVERSION}/build}

%if "%{_vendor}" == "suse"
%if %{!?KVER:1}%{?KVER:0}
%ifarch x86_64
%define flav debug default kdump smp xen
%else
%define flav bigsmp debug default kdump kdumppae smp vmi vmipae xen xenpae pae ppc64
%endif
%endif

%if %{!?KVER:0}%{?KVER:1}
%define flav %(echo %{KVER} | awk -F"-" '{print $3}')
%endif
%endif

%if %{IS_RHEL_VENDOR}
%if %{!?KVER:1}%{?KVER:0}
%define flav ""
%endif
%if %{!?KVER:0}%{?KVER:1}
%if "%{flav}" == ""
%define flav default
%endif
%endif
%endif

%{!?_name: %global _name mlnx-en}
%{!?_version: %global _version 5.2}
%{!?_release: %global _release 1.0.4.0.g13e6dd1}
%global _kmp_rel %{_release}%{?_kmp_build_num}%{?_dist}

%if %{PYTHON3}
%global mlnx_python_env    export MLNX_PYTHON_EXECUTABLE=%{_bindir}/python3
%global mlnx_python        python3
%else
%global mlnx_python_env    :
%global mlnx_python        python
%endif

Name: %{_name}
Group: System Environment/Kernel
Version: %{_version}
Release: %{_release}%{?_dist}
License: GPLv2
Url: http://www.mellanox.com
Vendor: Mellanox Technologies
Source0: %{_name}-%{_version}.tgz
Source1: mlx4.files
Provides: %{_name}
%if "%{KMP}" == "1"
Conflicts: mlnx_en
%endif
BuildRoot: %{?build_root:%{build_root}}%{!?build_root:/var/tmp/MLNX_EN}
Summary: mlnx-en kernel module(s)
%description
ConnectX Ehternet device driver
The driver sources are located at: http://www.mellanox.com/downloads/Drivers/mlnx-en-5.2-1.0.4.tgz

%package doc
Summary: Documentation for the Mellanox Ethernet Driver for Linux
Group: System/Kernel

%description doc
Documentation for the Mellanox Ethernet Driver for Linux
The driver sources are located at: http://www.mellanox.com/downloads/Drivers/mlnx-en-5.2-1.0.4.tgz

%package sources
Summary: Sources for the Mellanox Ethernet Driver for Linux
Group: System Environment/Libraries

%description sources
Sources for the Mellanox Ethernet Driver for Linux
The driver sources are located at: http://www.mellanox.com/downloads/Drivers/mlnx-en-5.2-1.0.4.tgz

%package utils
Summary: Utilities for the Mellanox Ethernet Driver for Linux
Group: System Environment/Libraries

%description utils
Utilities for the Mellanox Ethernet Driver for Linux
The driver sources are located at: http://www.mellanox.com/downloads/Drivers/mlnx-en-5.2-1.0.4.tgz

%package KMP
Summary: mlnx-en kernel module(s)
Group: System/Kernel
%description KMP
mlnx-en kernel module(s)
The driver sources are located at: http://www.mellanox.com/downloads/Drivers/mlnx-en-5.2-1.0.4.tgz

# build KMP rpms?
%if "%{KMP}" == "1"
%global kernel_release() $(make -C %{1} kernelrelease | grep -v make | tail -1)
BuildRequires: %kernel_module_package_buildreqs
BuildRequires: /usr/bin/perl
%(echo "Requires: mlnx-en-utils" > ${RPM_BUILD_ROOT}/preamble)
%kernel_module_package -f %{SOURCE1} %flav -p ${RPM_BUILD_ROOT}/preamble -r %{_kmp_rel}
%else # not KMP
%global kernel_source() %{KSRC}
%global kernel_release() %{KVERSION}
%global flavors_to_build default
%package -n mlnx_en
Version: %{_version}
Release: %{_release}.kver.%{krelver}
Requires: coreutils
Requires: kernel
Requires: pciutils
Requires: grep
Requires: procps
Requires: module-init-tools
Requires: mlnx-en-utils
Group: System Environment/Base
Summary: Ethernet NIC Driver
%description -n mlnx_en
ConnectX Ehternet device driver
The driver sources are located at: http://www.mellanox.com/downloads/Drivers/mlnx-en-5.2-1.0.4.tgz
%endif #end if "%{KMP}" == "1"

#
# setup module sign scripts if paths to the keys are given
#
%global WITH_MOD_SIGN %(if ( test -f "$MODULE_SIGN_PRIV_KEY" && test -f "$MODULE_SIGN_PUB_KEY" ); \
	then \
		echo -n '1'; \
	else \
		echo -n '0'; fi)

%if "%{WITH_MOD_SIGN}" == "1"
# call module sign script
%global __modsign_install_post \
    %{_builddir}/%{name}-%{version}/source/ofed_scripts/tools/sign-modules %{buildroot}/lib/modules/ %{kernel_source default} || exit 1 \
%{nil}

%global __debug_package 1
%global buildsubdir %{name}-%{version}
# Disgusting hack alert! We need to ensure we sign modules *after* all
# invocations of strip occur, which is in __debug_install_post if
# find-debuginfo.sh runs, and __os_install_post if not.
#
%global __spec_install_post \
  %{?__debug_package:%{__debug_install_post}} \
  %{__arch_install_post} \
  %{__os_install_post} \
  %{__modsign_install_post} \
%{nil}

%endif # end of setup module sign scripts
#

%if "%{_vendor}" == "suse"
%debug_package
%endif

%if %{IS_RHEL_VENDOR}
%global __find_requires %{nil}
%endif

%if "%{_vendor}" == "wrs" || "%{_vendor}" == "bluenix"
%global __python_provides %{nil}
%global __python_requires %{nil}
%endif

# set modules dir
%if %{IS_RHEL_VENDOR}
%if 0%{?fedora}
%global install_mod_dir updates
%else
%global install_mod_dir extra/%{name}
%endif
%endif

%if "%{_vendor}" == "suse"
%global install_mod_dir updates
%endif

%{!?install_mod_dir: %global install_mod_dir updates}

%prep
%setup
set -- *
mkdir source
mv "$@" source/
%if %{PYTHON3}
sed -s -i -e '1s|python\>|python3|' `grep -rl '^#!.*python' source/ofed_scripts`
%endif
mkdir obj

%build
rm -rf %{buildroot}
export EXTRA_CFLAGS='-DVERSION=\"%version\"'
%{mlnx_python_env}
for flavor in %{flavors_to_build}; do
	rm -rf obj/$flavor
	cp -r source obj/$flavor
	cd $PWD/obj/$flavor
	export KSRC=%{kernel_source $flavor}
	export KVERSION=%{kernel_release $KSRC}
	export MLNX_EN_PATCH_PARAMS="--kernel $KVERSION --kernel-sources $KSRC"
	%if "%{MEMTRACK}" == "1"
		export MLNX_EN_PATCH_PARAMS="$MLNX_EN_PATCH_PARAMS --with-memtrack"
	%endif
	%if "%{MLX5}" == "0"
		export MLNX_EN_PATCH_PARAMS="$MLNX_EN_PATCH_PARAMS --without-mlx5"
	%endif
	%if "%{MLXFW}" == "0"
		export MLNX_EN_PATCH_PARAMS="$MLNX_EN_PATCH_PARAMS --without-mlxfw"
	%endif
	find compat -type f -exec touch -t 200012201010 '{}' \; || true
	./scripts/mlnx_en_patch.sh $MLNX_EN_PATCH_PARAMS %{?_smp_mflags}
	make V=0 %{?_smp_mflags}
	cd -
done

cd source/ofed_scripts/utils
%{mlnx_python} setup.py build
cd -

%install
export INSTALL_MOD_PATH=%{buildroot}
export INSTALL_MOD_DIR=%{install_mod_dir}
%{mlnx_python_env}
for flavor in %{flavors_to_build}; do
	cd $PWD/obj/$flavor
	export KSRC=%{kernel_source $flavor}
	export KVERSION=%{kernel_release $KSRC}
	make install KSRC=$KSRC MODULES_DIR=$INSTALL_MOD_DIR DESTDIR=%{buildroot} KERNELRELEASE=$KVERSION
	# Cleanup unnecessary kernel-generated module dependency files.
	find $INSTALL_MOD_PATH/lib/modules -iname 'modules.*' -exec rm {} \;
	cd -
done

# Set the module(s) to be executable, so that they will be stripped when packaged.
find %{buildroot} \( -type f -name '*.ko' -o -name '*ko.gz' \) -exec %{__chmod} u+x \{\} \;

%if %{IS_RHEL_VENDOR}
%if ! 0%{?fedora}
%{__install} -d %{buildroot}%{_sysconfdir}/depmod.d/
for module in `find %{buildroot}/ -name '*.ko' -o -name '*.ko.gz' | sort`
do
ko_name=${module##*/}
mod_name=${ko_name/.ko*/}
mod_path=${module/*%{name}}
mod_path=${mod_path/\/${ko_name}}
echo "override ${mod_name} * weak-updates/%{name}${mod_path}" >> %{buildroot}%{_sysconfdir}/depmod.d/%{name}-${mod_name}.conf
echo "override ${mod_name} * extra/%{name}${mod_path}" >> %{buildroot}%{_sysconfdir}/depmod.d/%{name}-${mod_name}.conf
done
%endif
%endif

install -D -m 755 source/ofed_scripts/common_irq_affinity.sh %{buildroot}/%{_sbindir}/common_irq_affinity.sh
install -D -m 755 source/ofed_scripts/set_irq_affinity.sh %{buildroot}/%{_sbindir}/set_irq_affinity.sh
install -D -m 755 source/ofed_scripts/show_irq_affinity.sh %{buildroot}/%{_sbindir}/show_irq_affinity.sh
install -D -m 755 source/ofed_scripts/show_irq_affinity_hints.sh %{buildroot}/%{_sbindir}/show_irq_affinity_hints.sh
install -D -m 755 source/ofed_scripts/set_irq_affinity_bynode.sh %{buildroot}/%{_sbindir}/set_irq_affinity_bynode.sh
install -D -m 755 source/ofed_scripts/set_irq_affinity_cpulist.sh %{buildroot}/%{_sbindir}/set_irq_affinity_cpulist.sh
install -D -m 755 source/ofed_scripts/sysctl_perf_tuning %{buildroot}/sbin/sysctl_perf_tuning
install -D -m 755 source/ofed_scripts/mlnx_bf_configure %{buildroot}/sbin/mlnx_bf_configure
install -D -m 755 source/ofed_scripts/mlnx-sf %{buildroot}/sbin/mlnx-sf
install -D -m 755 source/ofed_scripts/mlnx_bf_udev %{buildroot}/lib/udev/mlnx_bf_udev
install -D -m 644 source/scripts/mlnx-en.conf %{buildroot}/etc/mlnx-en.conf
install -D -m 755 source/scripts/mlnx-en.d %{buildroot}/etc/init.d/mlnx-en.d
install -D -m 644 source/ofed_scripts/mlnx-bf.conf   %{buildroot}/etc/modprobe.d/mlnx-bf.conf

mkdir -p %{buildroot}/%{_prefix}/src
cp -r source %{buildroot}/%{_prefix}/src/%{name}-%{version}

touch ofed-files
cd source/ofed_scripts/utils
%{mlnx_python} setup.py install -O1 --root=%{buildroot} --record ../../../ofed-files
cd -

if [[ "$(ls %{buildroot}/%{_bindir}/tc_wrap.py* 2>/dev/null)" != "" ]]; then
	echo '%{_bindir}/tc_wrap.py*' >> ofed-files
fi

%if "%{WITH_SYSTEMD}" == "1"
install -D -m 644 source/scripts/mlnx-en.d.service %{buildroot}/%{_unitdir}/mlnx-en.d.service
%endif

# Update /etc/init.d/mlnx-en.d service header
is_euler=`grep 'NAME=".*Euler' /etc/os-release 2>/dev/null || :`
if [[ -f /etc/redhat-release || -f /etc/rocks-release || "$is_euler" != '' ]]; then
    perl -i -ne 'if (m@^#!/bin/bash@) {
        print q@#!/bin/bash
#
# Bring up/down mlnx-en.d
#
# chkconfig: 2345 05 95
# description: Activates/Deactivates mlnx-en Driver to \
#              start at boot time.
#
### BEGIN INIT INFO
# Provides:       mlnx-en.d
### END INIT INFO
@;
                 } else {
                     print;
                 }' %{buildroot}/etc/init.d/mlnx-en.d
fi

if [ -f /etc/SuSE-release ] || [ -f /etc/SUSE-brand ]; then
    local_fs='$local_fs'
    perl -i -ne "if (m@^#!/bin/bash@) {
        print q@#!/bin/bash
### BEGIN INIT INFO
# Provides:       mlnx-en.d
# Required-Start: $local_fs
# Required-Stop: 
# Default-Start:  2 3 5
# Default-Stop: 0 1 2 6
# Description:    Activates/Deactivates mlnx-en.d Driver to \
#                 start at boot time.
### END INIT INFO
@;
                 } else {
                     print;
                 }" %{buildroot}/etc/init.d/mlnx-en.d
fi

# Update mlnx-en.conf
%if "%{MLX5}" == "0"
	sed -i 's/MLX5_LOAD=yes/MLX5_LOAD=no/' %{buildroot}/etc/mlnx-en.conf
%endif

# end of install

%if "%{KMP}" != "1"
%post -n mlnx_en
/sbin/depmod -r -a %{KVERSION}
# W/A for OEL6.7/7.x inbox modules get locked in memory
# in dmesg we get: Module mlx4_core locked in memory until next boot
if (grep -qiE "Oracle.*(6.([7-9]|10)| 7)" /etc/issue /etc/*release* 2>/dev/null); then
	/sbin/dracut --force
fi

%postun -n mlnx_en
if [ $1 = 0 ]; then  # 1 : Erase, not upgrade
	/sbin/depmod -r -a %{KVERSION}
	# W/A for OEL6.7/7.x inbox modules get locked in memory
	# in dmesg we get: Module mlx4_core locked in memory until next boot
	if (grep -qiE "Oracle.*(6.([7-9]|10)| 7)" /etc/issue /etc/*release* 2>/dev/null); then
		/sbin/dracut --force
	fi
fi
%endif

%post -n mlnx-en-utils
is_euler=`grep 'NAME=".*Euler' /etc/os-release 2>/dev/null || :`
if [ $1 -eq 1 ]; then # 1 : This package is being installed
    if [[ -f /etc/redhat-release || -f /etc/rocks-release || "$is_euler" != '' ]]; then
        /sbin/chkconfig mlnx-en.d off >/dev/null 2>&1 || true
        /usr/bin/systemctl disable mlnx-en.d >/dev/null 2>&1 || true
        /sbin/chkconfig --del mlnx-en.d >/dev/null 2>&1 || true

%if "%{WITH_SYSTEMD}" != "1"
        /sbin/chkconfig --add mlnx-en.d >/dev/null 2>&1 || true
        /sbin/chkconfig mlnx-en.d on >/dev/null 2>&1 || true
%else
        /usr/bin/systemctl enable mlnx-en.d >/dev/null 2>&1 || true
%endif
    fi

    if [ -f /etc/SuSE-release ] || [ -f /etc/SUSE-brand ]; then
        /sbin/chkconfig mlnx-en.d off >/dev/null 2>&1 || true
        /usr/bin/systemctl disable mlnx-en.d >/dev/null 2>&1 || true
        /sbin/insserv -r mlnx-en.d >/dev/null 2>&1 || true

%if "%{WITH_SYSTEMD}" != "1"
        /sbin/insserv mlnx-en.d >/dev/null 2>&1 || true
        /sbin/chkconfig mlnx-en.d on >/dev/null 2>&1 || true
%else
        /usr/bin/systemctl enable mlnx-en.d >/dev/null 2>&1 || true
%endif
    fi

%if "%{WINDRIVER}" == "1" || "%{BLUENIX}" == "1"
    /usr/sbin/update-rc.d mlnx-en.d defaults || true
%endif

%if "%{POWERKVM}" == "1"
    /usr/bin/systemctl disable mlnx-en.d >/dev/null  2>&1 || true
    /usr/bin/systemctl enable mlnx-en.d >/dev/null  2>&1 || true
%endif

%if "%{WITH_SYSTEMD}" == "1"
    /usr/bin/systemctl daemon-reload >/dev/null 2>&1 || :
%endif

fi # 1 : closed
# END of post utils

%preun -n mlnx-en-utils
is_euler=`grep 'NAME=".*Euler' /etc/os-release 2>/dev/null || :`
if [ $1 = 0 ]; then  # 1 : Erase, not upgrade
    /sbin/chkconfig mlnx-en.d off >/dev/null 2>&1 || true
    /usr/bin/systemctl disable mlnx-en.d >/dev/null 2>&1 || true

    if [[ -f /etc/redhat-release || -f /etc/rocks-release || "$is_euler" != '' ]]; then
        /sbin/chkconfig --del mlnx-en.d  >/dev/null 2>&1 || true
    fi
    if [ -f /etc/SuSE-release ] || [ -f /etc/SUSE-brand ]; then
        /sbin/insserv -r mlnx-en.d >/dev/null 2>&1 || true
    fi

%if "%{WINDRIVER}" == "1" || "%{BLUENIX}" == "1"
    /usr/sbin/update-rc.d -f mlnx-en.d remove || true
%endif

%if "%{POWERKVM}" == "1"
    /usr/bin/systemctl disable mlnx-en.d >/dev/null  2>&1 || true
%endif

fi
# END of pre uninstall utils

%postun -n mlnx-en-utils
%if "%{WITH_SYSTEMD}" == "1"
/usr/bin/systemctl daemon-reload >/dev/null 2>&1 || :
%endif
#end of post uninstall

%clean
rm -rf %{buildroot}

%if "%{KMP}" != "1"
%files -n mlnx_en
/lib/modules/%{KVERSION}/%{install_mod_dir}/
%if %{IS_RHEL_VENDOR}
%if ! 0%{?fedora}
%config(noreplace) %{_sysconfdir}/depmod.d/%{name}-*.conf
%endif
%endif
%endif

%files doc
%defattr(-,root,root,-)

%files sources
%defattr(-,root,root,-)
%{_prefix}/src/%{name}-%{version}

%files utils -f ofed-files
%defattr(-,root,root,-)
%config(noreplace) /etc/modprobe.d/mlnx-bf.conf
%{_sbindir}/*
/sbin/*
/lib/udev/mlnx_bf_udev
%config(noreplace) /etc/mlnx-en.conf
/etc/init.d/mlnx-en.d
%if "%{WITH_SYSTEMD}" == "1"
%{_unitdir}/mlnx-en.d.service
%endif
%if "%{XENSERVER65}" == "1"
%config(noreplace) /etc/modprobe.d/mlnx_en.conf
%endif

%changelog
* Mon Mar 24 2014 Alaa Hleihel <alaa@mellanox.com>
- Use one source rpm for KMP and none-KMP rpms.
* Tue May 1 2012 Vladimir Sokolovsky <vlad@mellanox.com>
- Created spec file for mlnx_en
