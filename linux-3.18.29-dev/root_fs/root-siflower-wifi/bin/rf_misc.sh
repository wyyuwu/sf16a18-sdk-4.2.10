#!/bin/ash
set -e
#################################check if another core should be start up
p_num=`cat /proc/cpuinfo | grep "processor" | sort | uniq | wc -l`
if [ $p_num -eq 2 ]
then
modprobe startcore
fi



#################################RF set up
umount /sys/kernel/debug
mount -t debugfs none /sys/kernel/debug
modprobe sf16a18_rf
sleep 1

#################################umac set up
mxd=${mxd-0}

diag1=${diag1-1C}
diag2=${diag2-25}

freq=${freq-5180}
type=${type-managed}
txpower=500
#leg_rates="legacy-2.4 11 6 9 12"
#mcs_rates="ht-mcs-2.4 0 1 2 3 4 5 6 7"
bitrates="$leg_rates $mcs_rates"

hparams=${umhparams-}
UMH=${UMH-/bin/rwnx_umh.sh}

modparams="
agg_tx=${agg_tx-1}
amsdu_force=${amsdu_force-2}
ap_uapsd_on=${ap_uapsd_on-0}
autobcn=${autobcn-1}
bwsig_on=${bwsig_on-0}
cmon=${cmon-1}
custregd=${custregd-1}
dpsm=${dpsm-1}
dynbw_on=${dynbw_on-0}
gf_on=${gf_on-0}
hwscan=${hwscan-1}
phycfg=${phycfg-0}
ps_on=${ps_on-1}
sgi=${sgi-1}
uapsd_timeout=${uapsd_timeout-0}
use_2040=${use_2040-1}
vht_on=${vht_on-1}
dyndbg=${dyndbg-}
use_80=${use_80-1}
ht_on=${ht_on-1}
tx_lft=${tx_lft-100}
nss=${nss-1}
ldpc_on=${ldpc_on-1}
"
#tweak_bw=${tweak_bw-0}

if [ $1 == "all" ]
then
cmd="insmod sf16a18_lb_smac $modparams"
eval $cmd
cmd="insmod sf16a18_hb_smac $modparams"
eval $cmd || exit

else

cmd="insmod sf16a18_lb_smac $modparams"
if [ $1 == "hb" ]
then
cmd="insmod sf16a18_hb_smac $modparams"
fi
echo $cmd
eval $cmd || exit
fi
