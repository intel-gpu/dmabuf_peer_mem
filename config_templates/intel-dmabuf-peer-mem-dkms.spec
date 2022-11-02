%define module _MODULE_NAME_
%define version _VERSION_
%define release _RELEASE_

Summary: Intel RDMA Enablement Module
Name: %{module}
Version: %{version}
Release: %{release}
License: BSD
Group: System Environment/Base
BuildArch: x86_64
Vendor: Intel
Provides: %{module}
Packager: linux-graphics@intel.com
Requires: dkms gcc bash sed
# There is no Source# line for dkms.conf since it has been placed
# into the source tarball of SOURCE0
Source0: %{module}-%{version}-%{release}-src.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root/

%description
This RPM contains the Intel RDMA Enablement Module (MOFED Peer Memory plug-in
driver for DMA-BUF). Upon installation, dkms is invoked to build the driver
from source for kernels that MOFED has been installed for.

%prep
rm -rf %{module}-%{version}-%{release}
mkdir %{module}-%{version}-%{release}
cd %{module}-%{version}-%{release}
tar xvzf $RPM_SOURCE_DIR/%{module}-%{version}-%{release}-src.tar.gz

%install
if [ "%{buildroot}" != "/" ]; then
rm -rf %{buildroot}
fi
mkdir -p %{buildroot}/usr/src/%{module}-%{version}-%{release}/
cp -rf %{module}-%{version}-%{release}/* %{buildroot}/usr/src/%{module}-%{version}-%{release}

%clean
if [ "%{buildroot}" != "/" ]; then
rm -rf %{buildroot}
fi

%files
%defattr (-, root, root)
/usr/src/%{module}-%{version}-%{release}/

%pre

%post
/usr/sbin/dkms add -m %module -v %version-%release --rpm_safe_upgrade
for kernel_path in /lib/modules/*
do
    KERNEL=$(echo $kernel_path | cut -d '/' -f 4)
    MODULE=""
    if [ ! -d "${kernel_path}/build" ]; then
        echo "SKIP DKMS Installation: kernel Headers not available for variant $KERNEL"
        continue
    fi
    for module in $(find ${kernel_path} -name ib_core.ko)
    do
        grep -qs ib_register_peer_memory_client $module
        if [[ "$?" -eq 0 ]]; then
            MODULE=module
            break
        fi
    done
    if [ ! -z "${MODULE}" ]; then
        /usr/sbin/dkms install --force -m %module -v %version-%release -k $KERNEL
    else
        echo "SKIP DKMS Installation: MOFED installation not available for variant $KERNEL"
    fi
done
cp /usr/src/%{module}-%{version}-%{release}/udev/rules.d/92-dmabuf-peer-mem.rules /etc/udev/rules.d/
exit 0

%preun
echo -e
echo -e "Uninstall of %{module} module (version %{version}-%{release}) beginning:"
/usr/sbin/dkms remove -m %{module} -v %{version}-%{release} --all --rpm_safe_upgrade
if [ "$1" = 0 ] ; then
    rm -f /etc/udev/rules.d/92-dmabuf-peer-mem.rules
fi
exit 0
