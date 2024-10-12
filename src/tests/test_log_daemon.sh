#!/bin/bash

# Must disable auto bail
set +e

VALGRIND=
EXTRA_ARGS=
SOCKET=/tmp/rrr-logd-socket.sock
TMP=/tmp/rrr-logd-output
LOG_MSG="DEADBEEF"
LOG_PREFIX="test"
LOG_LEVEL="4"
LOG_OUT_4="<4> <$LOG_PREFIX> $LOG_MSG"
LOG_OUT_7="<7> <rrr_post> $LOG_MSG"
LOG_IN="$LOG_LEVEL	$LOG_PREFIX	$LOG_MSG	"
LOG_DEF="ustr#log_level,sep1,nsep#log_prefix,sep1,nsep#log_message,sep1"

if [ "x$VALGRIND" != "x" ]; then
	SLEEP=2
else
	SLEEP=0.2
fi

sigkill_and_bail() {
	PID=$1
	MSG=$2
	kill -SIGKILL $PID
	echo $MSG
	exit 1
}

function wait_and_verify() {
	PID=$1
	EXPECTED_RET=$2

	wait $PID
	RET=$?

	if [ $RET -ne $EXPECTED_RET ]; then
		echo "Unexpected return value $RET, expected $EXPECTED_RET"
		exit 1
	fi

	return 0
}

function verify_socket_deleted() {
	if [ -e $SOCKET ]; then
		echo "Socket $SOCKET still existed"
		exit 1
	fi

	return 0
}

function verify_socket_exists() {
	if [ -e $SOCKET ]; then
		echo "Socket $SOCKET still existed"
		exit 1
	fi

	return 0
}

####################################################
# Verify that logd terminates neatly
####################################################

rm -f $SOCKET || exit 1

$VALGRIND ../.libs/rrr_logd -s $SOCKET $EXTRA_ARGS &
PID=$!

sleep $SLEEP

if ! [ -S $SOCKET ]; then
	sigkill_and_bail $PID "Socket $SOCKET did not exist"
fi

kill -SIGUSR1 $PID || exit $?
wait_and_verify $PID 0
RET=$?

verify_socket_deleted

####################################################
# Verify that incorrect fd produces error
####################################################

$VALGRIND ../.libs/rrr_logd -f 666 $EXTRA_ARGS &
PID=$!

sleep $SLEEP

kill -SIGKILL $PID > /dev/null 2>&1

wait_and_verify $PID 1

function log_delivery() {
	IN=$1
	POST_ARGS=$2
	OUT=$3

	# Don't add $EXTRA_ARGS here, clobbers output
	$VALGRIND ../.libs/rrr_logd -p -s $SOCKET > $TMP &
	PID=$!

	sleep $SLEEP

	# Test using specified array definition
	if ! echo "$IN" | ../.libs/rrr_post $SOCKET $POST_ARGS -c 1 -f - $EXTRA_ARGS; then
		sigkill_and_bail $PID "Failed to post log messages"
		exit 1
	fi

	sleep $SLEEP

	kill -SIGUSR1 $PID || exit $?
	wait_and_verify $PID 0
	RET=$?

	verify_socket_deleted

	OUTPUT=`cat $TMP`

	if [ "x$OUTPUT" != "x$OUT" ]; then
		echo "Unexpected output '$OUTPUT' expected '$OUT'"
		rm -f $TMP
		exit 1
	fi

	rm -f $TMP
}

####################################################
# Verify array log message delivery on socket
####################################################

log_delivery "$LOG_IN" "-a $LOG_DEF" "$LOG_OUT_4" 

####################################################
# Verify native log message delivery on socket
####################################################

log_delivery "$LOG_MSG" "-L" "$LOG_OUT_7" 
