#!/bin/bash
#
# $VUFIND_HOME and $VUFIND_LOCAL_DIR have to be set!
#
# Creates all directories and the vufind user.
# Sets all file privileges of VuFind and all other needed files.
#
set -o errexit -o nounset
SCRIPT_DIR=$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )

# Make sure only root can run our script
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root" 1>&2
   exit 1
fi

show_help() {
  cat << EOF
Creates all directories and the vufind user. Sets all file privileges of VuFind and all other needed files.

USAGE: ${0##*/} CLONE_DIRECTORY USER_NAME USER_GROUP

CLONE_DIRECTORY  The name of the subdirectory for the cloned repository
USER_NAME    The user name of the vufind user
USER_GROUP   The user group of the vufind user
EOF
}

if [ "$#" -ne 3 ] ; then
  show_help
  exit 1
fi

CLONE_DIRECTORY=$1
USER_NAME=$2
USER_GROUP=$3
OWNER="$USER_NAME:$USER_GROUP"

chmod +xr "$VUFIND_HOME"
chmod +xr "$VUFIND_LOCAL_DIR"
touch "$VUFIND_LOCAL_DIR/logs/record.xml"
touch "$VUFIND_LOCAL_DIR/logs/search.xml"

touch "$VUFIND_LOCAL_DIR/import/solrmarc.log"
mkdir --parents "$VUFIND_LOCAL_DIR/config/vufind/local_overrides"
chmod +xr "$VUFIND_LOCAL_DIR/config/vufind/local_overrides"

chown -R "$OWNER" "$VUFIND_HOME"
chown -R "$OWNER" "$CLONE_DIRECTORY"

touch "/var/log/vufind.log"
chown "$OWNER" "/var/log/vufind.log"

mkdir --parents "/tmp/vufind_sessions/"
chown -R "$OWNER" "/tmp/vufind_sessions/"

mkdir --parents "/var/lib/tuelib/bibleRef"
chown -R "$OWNER" "/var/lib/tuelib"

if [[ $(which getenforce) && $(getenforce) == "Enforcing" ]] ; then

  if [[ $(which setsebool) ]]; then
	  setsebool -P httpd_can_network_connect=1 \
               httpd_can_network_connect_db=1 \
               httpd_enable_cgi=1
  fi

  if [[ $(which chcon) ]]; then
    chcon --recursive unconfined_u:object_r:httpd_sys_rw_content_t:s0 "$VUFIND_HOME"
    chcon system_u:object_r:httpd_config_t:s0 "$VUFIND_LOCAL_DIR/httpd-vufind.conf"
    chcon system_u:object_r:httpd_config_t:s0 "$VUFIND_LOCAL_DIR/httpd-vufind-vhosts.conf"
    chcon unconfined_u:object_r:httpd_sys_rw_content_t:s0 "$VUFIND_LOCAL_DIR/logs/record.xml"
    chcon unconfined_u:object_r:httpd_sys_rw_content_t:s0 "$VUFIND_LOCAL_DIR/logs/search.xml"
    chcon system_u:object_r:httpd_log_t:s0 /var/log/vufind.log
  fi

else
  echo "##########################################################################################"
  echo "# WARNING: SELinux is either not properly installed or currently disabled on this system #" 
  echo "# Skipped SELinux configuration...							 #"
  echo "##########################################################################################"
fi

