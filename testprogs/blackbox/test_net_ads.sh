if [ $# -lt 4 ]; then
cat <<EOF
Usage: test_net.sh DC_SERVER DC_USERNAME DC_PASSWORD PREFIX_ABS
EOF
exit 1;
fi

DC_SERVER=$1
DC_USERNAME=$2
DC_PASSWORD=$3
BASEDIR=$4

HOSTNAME=`dd if=/dev/urandom bs=1 count=32 2>/dev/null | sha1sum | cut -b 1-10`

RUNDIR=`pwd`
cd $BASEDIR
WORKDIR=`mktemp -d -p .`
WORKDIR=`basename $WORKDIR`
cp -a client/* $WORKDIR/
sed -ri "s@(dir|directory) = (.*)/client/@\1 = \2/$WORKDIR/@" $WORKDIR/client.conf
sed -ri "s/netbios name = .*/netbios name = $HOSTNAME/" $WORKDIR/client.conf
rm -f $WORKDIR/private/secrets.tdb
cd $RUNDIR

failed=0

net_tool="$BINDIR/net -s $BASEDIR/$WORKDIR/client.conf --option=security=ads"

ldbsearch="ldbsearch"
if [ -x "$BINDIR/ldbsearch" ]; then
	ldbsearch="$BINDIR/ldbsearch"
fi

# Load test functions
. `dirname $0`/subunit.sh

testit "join" $VALGRIND $net_tool ads join -U$DC_USERNAME%$DC_PASSWORD || failed=`expr $failed + 1`

testit "testjoin" $VALGRIND $net_tool ads testjoin -kP || failed=`expr $failed + 1`

netbios=$(grep "netbios name" $BASEDIR/$WORKDIR/client.conf | cut -f2 -d= | awk '{$1=$1};1')

testit "test setspn list $netbios" $VALGRIND $net_tool ads setspn list $netbios -U$DC_USERNAME%$DC_PASSWORD || failed=`expr $failed + 1`
spn="foo"
testit_expect_failure "test setspn add illegal windows spn ($spn)" $VALGRIND $net_tool ads setspn add $spn -U$DC_USERNAME%$DC_PASSWORD || failed=`expr $failed + 1`

spn="foo/somehost.domain.com"
testit "test setspn add ($spn)" $VALGRIND $net_tool ads setspn add $spn -U$DC_USERNAME%$DC_PASSWORD || failed=`expr $failed + 1`

found=$($net_tool ads setspn list -U$DC_USERNAME%$DC_PASSWORD | grep $spn | wc -l)
testit "test setspn list shows the newly added spn ($spn)" test $found -eq 1 || failed=`expr $failed + 1`

up_spn=$(echo $spn | tr '[:lower:]' '[:upper:]')
testit_expect_failure "test setspn add existing (case-insensitive) spn ($spn)" $VALGRIND $net_tool ads setspn add $up_spn -U$DC_USERNAME%$DC_PASSWORD || failed=`expr $failed + 1`

testit "test setspn delete existing (case-insensitive) ($spn)" $VALGRIND $net_tool ads setspn delete $spn  -U$DC_USERNAME%$DC_PASSWORD || failed=`expr $failed + 1`

found=$($net_tool ads setspn list  -U$DC_USERNAME%$DC_PASSWORD | grep $spn | wc -l)
testit "test setspn list shows the newly deleted spn ($spn) is gone" test $found -eq 0 || failed=`expr $failed + 1`

testit "changetrustpw" $VALGRIND $net_tool ads changetrustpw || failed=`expr $failed + 1`

testit "leave" $VALGRIND $net_tool ads leave -U$DC_USERNAME%$DC_PASSWORD || failed=`expr $failed + 1`

# Test with kerberos method = secrets and keytab
dedicated_keytab_file="$PREFIX_ABS/test_net_ads_dedicated_krb5.keytab"
testit "join (dedicated keytab)" $VALGRIND $net_tool ads join -U$DC_USERNAME%$DC_PASSWORD --option="kerberosmethod=dedicatedkeytab" --option="dedicatedkeytabfile=$dedicated_keytab_file" || failed=`expr $failed + 1`

testit "testjoin (dedicated keytab)" $VALGRIND $net_tool ads testjoin -kP || failed=`expr $failed + 1`

netbios=$(grep "netbios name" $BASEDIR/$WORKDIR/client.conf | cut -f2 -d= | awk '{$1=$1};1')
uc_netbios=$(echo $netbios | tr '[:lower:]' '[:upper:]')
lc_realm=$(echo $REALM | tr '[:upper:]' '[:lower:]')
fqdns="$netbios.$lc_realm"

krb_princ="primary/instance@$REALM"
testit "test (dedicated keytab) add a fully qualified krb5 principal" $VALGRIND $net_tool ads keytab add $krb_princ -U$DC_USERNAME%$DC_PASSWORD --option="kerberosmethod=dedicatedkeytab" --option="dedicatedkeytabfile=$dedicated_keytab_file" || failed=`expr $failed + 1`

found=`$net_tool ads keytab list -U$DC_USERNAME%$DC_PASSWORD --option="kerberosmethod=dedicatedkeytab" --option="dedicatedkeytabfile=$dedicated_keytab_file" | grep $krb_princ | wc -l`

testit "test (dedicated keytab) at least one fully qualified krb5 principal that was added is present in keytab" test $found -gt 1 || failed=`expr $failed + 1`

machinename="machine123"
testit "test (dedicated keytab) add a kerberos prinicple created from machinename to keytab" $VALGRIND $net_tool ads keytab add $machinename'$' -U$DC_USERNAME%$DC_PASSWORD --option="kerberosmethod=dedicatedkeytab" --option="dedicatedkeytabfile=$dedicated_keytab_file" || failed=`expr $failed + 1`
search_str="$machinename\$@$REALM"
found=`$net_tool ads keytab list -U$DC_USERNAME%$DC_PASSWORD --option="kerberosmethod=dedicatedkeytab" --option="dedicatedkeytabfile=$dedicated_keytab_file" | grep $search_str | wc -l`
testit "test (dedicated keytab) at least one krb5 principal created from $machinename added is present in keytab" test $found -gt 1 || failed=`expr $failed + 1`

service="nfs"
testit "test (dedicated keytab) add a $service service to keytab" $VALGRIND $net_tool ads keytab add $service -U$DC_USERNAME%$DC_PASSWORD --option="kerberosmethod=dedicatedkeytab" --option="dedicatedkeytabfile=$dedicated_keytab_file" || failed=`expr $failed + 1`

search_str="$service/$fqdns@$REALM"
found=`$net_tool ads keytab list -U$DC_USERNAME%$DC_PASSWORD --option="kerberosmethod=dedicatedkeytab" --option="dedicatedkeytabfile=$dedicated_keytab_file" | grep $search_str | wc -l`
testit "test (dedicated keytab) at least one (long form) krb5 principal created from service added is present in keytab" test $found -gt 1 || failed=`expr $failed + 1`

search_str="$service/$uc_netbios@$REALM"
found=`$net_tool ads keytab list -U$DC_USERNAME%$DC_PASSWORD --option="kerberosmethod=dedicatedkeytab" --option="dedicatedkeytabfile=$dedicated_keytab_file" | grep $search_str | wc -l`
testit "test (dedicated keytab) at least one (shorter form) krb5 principal created from service added is present in keytab" test $found -gt 1 || failed=`expr $failed + 1`

spn_service="random_srv"
spn_host="somehost.subdomain.domain"
spn_port="12345"

windows_spn="$spn_service/$spn_host"
testit "test (dedicated keytab) add a $windows_spn windows style SPN to keytab" $VALGRIND $net_tool ads keytab add $windows_spn -U$DC_USERNAME%$DC_PASSWORD --option="kerberosmethod=dedicatedkeytab" --option="dedicatedkeytabfile=$dedicated_keytab_file" || failed=`expr $failed + 1`

search_str="$spn_service/$spn_host@$REALM"
found=`$net_tool ads keytab list -U$DC_USERNAME%$DC_PASSWORD --option="kerberosmethod=dedicatedkeytab" --option="dedicatedkeytabfile=$dedicated_keytab_file" | grep $search_str | wc -l`
testit "test (dedicated keytab) at least one krb5 principal created from windown SPN added is present in keytab" test $found -gt 1 || failed=`expr $failed + 1`

windows_spn="$spn_service/$spn_host:$spn_port"
testit "test (dedicated keytab) add a $windows_spn windows style SPN to keytab" $VALGRIND $net_tool ads keytab add $windows_spn -U$DC_USERNAME%$DC_PASSWORD --option="kerberosmethod=dedicatedkeytab" --option="dedicatedkeytabfile=$dedicated_keytab_file" || failed=`expr $failed + 1`

search_str="$spn_service/$spn_host@$REALM"
found=`$net_tool ads keytab list -U$DC_USERNAME%$DC_PASSWORD --option="kerberosmethod=dedicatedkeytab" --option="dedicatedkeytabfile=$dedicated_keytab_file" | grep $search_str | wc -l`
testit "test (dedicated keytab) at least one krb5 principal created from windown SPN (with port) added is present in keytab" test $found -gt 1 || failed=`expr $failed + 1`

# keytab add shouldn't have written spn to AD
found=$($net_tool ads setspn list -U$DC_USERNAME%$DC_PASSWORD | grep $service | wc -l)
testit "test (dedicated keytab) spn is not written to AD (using keytab add)" test $found -eq 0 || failed=`expr $failed + 1`

ad_service="writetoad"
testit "test (dedicated keytab) add a $ad_service service to keytab (using add_update_ads" $VALGRIND $net_tool ads keytab add_update_ads $ad_service -U$DC_USERNAME%$DC_PASSWORD --option="kerberosmethod=dedicatedkeytab" --option="dedicatedkeytabfile=$dedicated_keytab_file" || failed=`expr $failed + 1`

found=$($net_tool ads setspn list -U$DC_USERNAME%$DC_PASSWORD | grep $ad_service | wc -l)
testit "test (dedicated keytab) spn is written to AD (using keytab add_update_ads)" test $found -eq 2 || failed=`expr $failed + 1`


# test existence in keytab of service (previously added) pulled from SPN post
# 'keytab create' is now present in keytab file
testit "test (dedicated keytab) keytab created succeeds" $VALGRIND $net_tool ads keytab create -U$DC_USERNAME%$DC_PASSWORD --option="kerberosmethod=dedicatedkeytab" --option="dedicatedkeytabfile=$dedicated_keytab_file" || failed=`expr $failed + 1`
found=$($net_tool ads keytab list -U$DC_USERNAME%$DC_PASSWORD --option="kerberosmethod=dedicatedkeytab" --option="dedicatedkeytabfile=$dedicated_keytab_file" | grep $ad_service | wc -l)
testit "test (dedicated keytab) spn service that exists in AD (created via add_update_ads) is added to keytab file" test $found -gt 1 || failed=`expr $failed + 1`

found_ad=$($net_tool ads setspn list -U$DC_USERNAME%$DC_PASSWORD | grep $service | wc -l)
found_keytab=$($net_tool ads keytab list -U$DC_USERNAME%$DC_PASSWORD  --option="kerberosmethod=dedicatedkeytab" --option="dedicatedkeytabfile=$dedicated_keytab_file" | grep $service | wc -l)
# test after create that a spn that exists in the keytab but shouldn't
# be written to the AD.
testit "test spn service doensn't exist in AD but is present in keytab file after keytab create" test $found_ad -eq 0 -a $found_keytab -gt 1 || failed=`expr $failed + 1`

# SPN parser is very basic but does detect some illegal combination

windows_spn="$spn_service/$spn_host:"
testit_expect_failure "test (dedicated keytab) fail to parse windows spn with missing port" $VALGRIND $net_tool ads keytab add $windows_spn -U$DC_USERNAME%$DC_PASSWORD --option="kerberosmethod=dedicatedkeytab" --option="dedicatedkeytabfile=$dedicated_keytab_file" || failed=`expr $failed + 1

windows_spn="$spn_service/$spn_host/"
testit_expect_failure "test (dedicated keytab) fail to parse windows spn with missing servicename" $VALGRIND $net_tool ads keytab add $windows_spn -U$DC_USERNAME%$DC_PASSWORD --option="kerberosmethod=dedicatedkeytab" --option="dedicatedkeytabfile=$dedicated_keytab_file" || failed=`expr $failed + 1

testit "changetrustpw (dedicated keytab)" $VALGRIND $net_tool ads changetrustpw || failed=`expr $failed + 1`

testit "leave (dedicated keytab)" $VALGRIND $net_tool ads leave -U$DC_USERNAME%$DC_PASSWORD || failed=`expr $failed + 1`

# if there is no keytab, try and create it
if [ ! -f $dedicated_keytab_file ]; then
  if [ $(command -v ktutil) >/dev/null ]; then
    printf "addent -password -p $DC_USERNAME@$REALM -k 1 -e rc4-hmac\n$DC_PASSWORD\nwkt $dedicated_keytab_file\n" | ktutil
  fi
fi

if [  -f $dedicated_keytab_file ]; then
  testit "keytab list (dedicated keytab)" $VALGRIND $net_tool ads keytab list --option="kerberosmethod=dedicatedkeytab" --option="dedicatedkeytabfile=$dedicated_keytab_file" || failed=`expr $failed + 1`
  testit "keytab list keytab specified on cmdline" $VALGRIND $net_tool ads keytab list $dedicated_keytab_file || failed=`expr $failed + 1`
fi

rm -f $dedicated_keytab_file

testit_expect_failure "testjoin(not joined)" $VALGRIND $net_tool ads testjoin -kP || failed=`expr $failed + 1`

testit "join+kerberos" $VALGRIND $net_tool ads join -kU$DC_USERNAME%$DC_PASSWORD || failed=`expr $failed + 1`

testit "testjoin" $VALGRIND $net_tool ads testjoin -kP || failed=`expr $failed + 1`

testit "leave+kerberos" $VALGRIND $net_tool ads leave -kU$DC_USERNAME%$DC_PASSWORD || failed=`expr $failed + 1`

testit_expect_failure "testjoin(not joined)" $VALGRIND $net_tool ads testjoin -kP || failed=`expr $failed + 1`

testit "join+server" $VALGRIND $net_tool ads join -U$DC_USERNAME%$DC_PASSWORD -S$DC_SERVER || failed=`expr $failed + 1`

testit "leave+server" $VALGRIND $net_tool ads leave -U$DC_USERNAME%$DC_PASSWORD -S$DC_SERVER || failed=`expr $failed + 1`

testit_expect_failure "join+invalid_server" $VALGRIND $net_tool ads join -U$DC_USERNAME%$DC_PASSWORD -SINVALID && failed=`expr $failed + 1`

testit "join+server" $VALGRIND $net_tool ads join -U$DC_USERNAME%$DC_PASSWORD || failed=`expr $failed + 1`

testit_expect_failure "leave+invalid_server" $VALGRIND $net_tool ads leave -U$DC_USERNAME%$DC_PASSWORD -SINVALID && failed=`expr $failed + 1`

testit "testjoin user+password" $VALGRIND $net_tool ads testjoin -U$DC_USERNAME%$DC_PASSWORD || failed=`expr $failed + 1`

testit "leave+keep_account" $VALGRIND $net_tool ads leave -U$DC_USERNAME%$DC_PASSWORD --keep-account || failed=`expr $failed + 1`

computers_ldb_ou="CN=Computers,DC=addom,DC=samba,DC=example,DC=com"
testit "ldb check for existence of machine account" $ldbsearch -U$DC_USERNAME%$DC_PASSWORD -H ldap://$SERVER.$REALM -s base -b "cn=$HOSTNAME,$computers_ldb_ou" || failed=`expr $failed + 1`

testit "join" $VALGRIND $net_tool ads join -U$DC_USERNAME%$DC_PASSWORD || failed=`expr $failed + 1`

testit "testjoin" $VALGRIND $net_tool ads testjoin || failed=`expr $failed + 1`

##Goodbye...
testit "leave" $VALGRIND $net_tool ads leave -U$DC_USERNAME%$DC_PASSWORD || failed=`expr $failed + 1`

rm -rf $BASEDIR/$WORKDIR

exit $failed
