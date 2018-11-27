#!/bin/bash
#
# Copyright (c) 2016 Mellanox Technologies. All rights reserved.
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
# Author: Alaa Hleihel <alaa@mellanox.com>
#

i=$1
shift

if [ -z "$i" ]; then
    echo "Usage:"
    echo "      $0 <interface>"
    exit 1
fi


KNOWN_CONF_VARS="TYPE BROADCAST MASTER BRIDGE BOOTPROTO IPADDR NETMASK PREFIX \
                 NAME DEVICE ONBOOT NM_CONTROLLED CONNECTED_MODE"

OPENIBD_CONFIG=${OPENIBD_CONFIG:-"/etc/infiniband/openib.conf"}
CONFIG=$OPENIBD_CONFIG
export LANG=en_US.UTF-8

if [ ! -f $CONFIG ]; then
    echo No InfiniBand configuration found
    exit 0
fi

OS_IS_BOOTING=0
last_bootID=$(cat /var/run/mlx_ifc-${i}.bootid 2>/dev/null)
bootID=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null | sed -e 's/-//g')
echo $bootID > /var/run/mlx_ifc-${i}.bootid
if [[ "X$last_bootID" == "X" || "X$last_bootID" != "X$bootID" ]]; then
    OS_IS_BOOTING=1
fi

. $CONFIG
IPOIB_MTU=${IPOIB_MTU:-65520}

if [ -f /etc/redhat-release ]; then
    NETWORK_CONF_DIR="/etc/sysconfig/network-scripts"
elif [ -f /etc/rocks-release ]; then
    NETWORK_CONF_DIR="/etc/sysconfig/network-scripts"
elif [ -f /etc/SuSE-release ]; then
    NETWORK_CONF_DIR="/etc/sysconfig/network"
else
    if [ -d /etc/sysconfig/network-scripts ]; then
        NETWORK_CONF_DIR="/etc/sysconfig/network-scripts"
    elif [ -d /etc/sysconfig/network ]; then
        NETWORK_CONF_DIR="/etc/sysconfig/network"
    fi
fi

log_msg()
{
    logger -t 'mlnx_interface_mgr' -i "$@"
}

set_ipoib_cm()
{
    local i=$1
    shift
    local mtu=$1
    shift
    local is_up=""
    local RC=0

    if [ ! -e /sys/class/net/${i}/mode ]; then
        log_msg "Failed to configure IPoIB connected mode for ${i}"
        return 1
    fi

    mtu=${mtu:-$IPOIB_MTU}

    #check what was the previous state of the interface
    is_up=`/sbin/ip link show $i | grep -w UP`

    /sbin/ip link set ${i} down
    if [ $? -ne 0 ]; then
        log_msg "set_ipoib_cm: Failed to bring down ${i} in order to change connection mode"
        return 1
    fi

    if [ -w /sys/class/net/${i}/mode ]; then
        echo connected > /sys/class/net/${i}/mode
        if [ $? -eq 0 ]; then
            log_msg "set_ipoib_cm: ${i} connection mode set to connected"
        else
            log_msg "set_ipoib_cm: Failed to change connection mode for ${i} to connected"
            RC=1
        fi
    else
        log_msg "set_ipoib_cm: cannot write to /sys/class/net/${i}/mode"
        RC=1
    fi
    /sbin/ip link set ${i} mtu ${mtu}
    if [ $? -ne 0 ]; then
        log_msg "set_ipoib_cm: Failed to set mtu for ${i}"
        RC=1
    fi

    #if the intf was up returns it to
    if [ -n "$is_up" ]; then
        /sbin/ip link set ${i} up
        if [ $? -ne 0 ]; then
            log_msg "set_ipoib_cm: Failed to bring up ${i} after setting connection mode to connected"
            RC=1
        fi
    fi

    return $RC
}

set_RPS_cpu()
{
    local i=$1
    shift

    # Silently ignore Pkeys
    if [ ! -e /sys/class/net/${i}/device ]; then
        return 0
    fi

    if [ ! -e /sys/class/net/${i}/queues/rx-0/rps_cpus ]; then
        log_msg "set_RPS_cpu: Failed to configure RPS cpu for ${i}"
        return 1
    fi

    cat /sys/class/net/${i}/device/local_cpus >  /sys/class/net/${i}/queues/rx-0/rps_cpus
    if [ $? -eq 0 ]; then
        log_msg "set_RPS_cpu: Configured RPS cpu for ${i}"
    else
        log_msg "set_RPS_cpu: Failed to configure RPS cpu for ${i}"
        return 1
    fi

    return 0
}

bring_up()
{
    local i=$1
    shift
    local RC=0

    local IFCFG_FILE="${NETWORK_CONF_DIR}/ifcfg-${i}"
    # W/A for conf files created with nmcli
    if [ ! -e "$IFCFG_FILE" ]; then
        IFCFG_FILE=$(grep -E "=\s*\"*${i}\"*\s*\$" ${NETWORK_CONF_DIR}/* 2>/dev/null | head -1 | cut -d":" -f"1")
    fi

    if [ -e "${IFCFG_FILE}" ]; then
        . ${IFCFG_FILE}
        if [ "${ONBOOT}" = "no" -o "${ONBOOT}" = "NO" ] && [ $OS_IS_BOOTING -eq 1 ]; then
            log_msg "interface $i has ONBOOT=no, skipping."
            unset $KNOWN_CONF_VARS
            return 5
        fi
    fi

    # Take CM mode settings from ifcfg file if set,
    # otherwise, take it from openib.conf
    local SET_CONNECTED_MODE=${CONNECTED_MODE:-$SET_IPOIB_CM}

    # relevant for IPoIB interfaces only
    if (/sbin/ethtool -i ${i} 2>/dev/null | grep -q ib_ipoib); then
        if [ "X${SET_CONNECTED_MODE}" == "Xyes" ]; then
            set_ipoib_cm ${i} ${MTU}
            if [ $? -ne 0 ]; then
                RC=1
            fi
        elif [ "X${SET_CONNECTED_MODE}" == "Xauto" ]; then
            # handle mlx5 interfaces, assumption: mlx5 interface will be with CM mode.
            if [ "X$(basename `readlink -f /sys/class/net/${i}/device/driver/module 2>/dev/null` 2>/dev/null)" == "Xmlx5_core" ]; then
                set_ipoib_cm ${i} ${MTU}
                if [ $? -ne 0 ]; then
                    RC=1
                fi
            fi
        fi
        # Spread the one and only RX queue to more CPUs using RPS.
        local num_rx_queue=$(ls -l /sys/class/net/${i}/queues/ 2>/dev/null | grep rx-  | wc -l | awk '{print $1}')
        if [ $num_rx_queue -eq 1 ]; then
            set_RPS_cpu ${i}
            if [ $? -ne 0 ]; then
                RC=1
            fi
        fi
    fi

    if [ ! -e "${IFCFG_FILE}" ]; then
        log_msg "No configuration found for ${i}"
        unset $KNOWN_CONF_VARS
        return 4
    fi

    /sbin/ifup ${i} >/dev/null 2>&1
    if [ $? -eq 0 ]; then
        log_msg "Bringing up interface $i: PASSED"
    else
        log_msg "Bringing up interface $i: FAILED"
        unset $KNOWN_CONF_VARS
        return 1
    fi

    if [ "X$MASTER" != "X" ]; then
        log_msg "$i - briging up bond master: $MASTER ..."
        local is_up=`/sbin/ip link show $MASTER | grep -w UP`
        if [ -z "$is_up" ]; then
            /sbin/ifup $MASTER >dev/null 2>&1
            if [ $? -ne 0 ]; then
                log_msg "$i - briging up bond master $MASTER: PASSED"
            else
                log_msg "$i - briging up bond master $MASTER: FAILED"
                RC=1
            fi
        else
                log_msg "$i - bond master $MASTER is already up"
        fi
    fi

    # bring up the relevant bridge interface
    if [ "X$BRIDGE" != "X" ]; then
        log_msg "$i - briging up bridge interface: $BRIDGE ..."
        /sbin/ifup $BRIDGE >dev/null 2>&1
        if [ $? -ne 0 ]; then
            log_msg "$i - briging up bridge interface $BRIDGE: PASSED"
        else
            log_msg "$i - briging up bridge interface $BRIDGE: FAILED"
            RC=1
        fi
    fi

    unset $KNOWN_CONF_VARS
    return $RC
}

# main
log_msg "Setting up Mellanox network interface: $i"

# bring up the interface
bring_up $i
if [ $? -eq 1 ]; then
    log_msg "Couldn't fully configure ${i}, review system logs and restart network service after fixing the issues."
fi

case "$(echo ${i} | tr '[:upper:]' '[:lower:]')" in
    *ib* | *infiniband*)
    ############################ IPoIB (Pkeys) ####################################
    # get list of conf child interfaces conf files
    CHILD_CONFS=$(/bin/ls -1 ${NETWORK_CONF_DIR}/ifcfg-${i}.[0-9]* 2> /dev/null)
    # W/A for conf files created with nmcli
    for ff in $(grep -E "=\s*\"*${i}\.[0-9]*\"*\s*\$" ${NETWORK_CONF_DIR}/* 2>/dev/null | cut -d":" -f"1")
    do
        if $(echo ${CHILD_CONFS} 2>/dev/null | grep -q ${ff}); then
            continue
        fi
        CHILD_CONFS="${CHILD_CONFS} ${ff}"
    done

    # Bring up child interfaces if configured.
    for child_conf in ${CHILD_CONFS}
    do
        ch_i=${child_conf##*-}
        # Skip saved interfaces rpmsave and rpmnew
        if (echo $ch_i | grep rpm > /dev/null 2>&1); then
            continue
        fi

        if [ ! -f /sys/class/net/${i}/create_child ]; then
            continue
        fi

        suffix=$(echo ${ch_i##*.} | tr '[:upper:]' '[:lower:]')
        if [[ ${suffix} =~ ^[0-9a-f]{1,4}$ ]]; then
            hexa=$(printf "%x" $(( 0x${suffix} | 0x8000 )))
            if [[ ${hexa} != ${suffix} ]]; then
                log_msg "Error: MSB is NOT set for pkey ${suffix} (should be ${hexa}); skipping interface ${ch_i}."
                continue
            fi
        else
            log_msg "Error: pkey ${suffix} is not hexadecimal (maximum 4 digits); skipping."
            continue
        fi
        pkey=0x${hexa}

        if [ ! -e /sys/class/net/${i}.${ch_i##*.} ] ; then
            {
            local retry_cnt=0
            echo $pkey > /sys/class/net/${i}/create_child
            while [[ $? -ne 0 && $retry_cnt -lt 10 ]]; do
                sleep 1
                let retry_cnt++
                echo $pkey > /sys/class/net/${i}/create_child
            done
            } > /dev/null 2>&1
        fi

        bring_up $ch_i
        if [ $? -eq 1 ]; then
            log_msg "Couldn't fully configure ${ch_i}, review system logs and restart network service after fixing the issues."
        fi
    done
    ############################ End of IPoIB (Pkeys) ####################################
    ;;
    *)
    ########################### Ethernet  (Vlans) #################################
    # get list of conf child interfaces conf files
    CHILD_CONFS=$(/bin/ls -1 ${NETWORK_CONF_DIR}/ifcfg-${i}.[0-9]* 2> /dev/null)
    # W/A for conf files created with nmcli
    for ff in $(grep -E "=\s*\"*${i}\.[0-9]*\"*\s*\$" ${NETWORK_CONF_DIR}/* 2>/dev/null | cut -d":" -f"1")
    do
        if $(echo ${CHILD_CONFS} 2>/dev/null | grep -q ${ff}); then
            continue
        fi
        CHILD_CONFS="${CHILD_CONFS} ${ff}"
    done

    # Bring up child interfaces if configured.
    for child_conf in ${CHILD_CONFS}
    do
        ch_i=${child_conf##*-}
        # Skip saved interfaces rpmsave and rpmnew
        if (echo $ch_i | grep rpm > /dev/null 2>&1); then
            continue
        fi

        bring_up $ch_i
        if [ $? -eq 1 ]; then
            log_msg "Couldn't fully configure ${ch_i}, review system logs and restart network service after fixing the issues."
        fi
    done
    ########################### End of Ethernet  (Vlans) #################################
    ;;
esac
