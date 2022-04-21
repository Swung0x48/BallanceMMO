#! /bin/bash
function archive_logs () {
    gzip -v log_$current_time.log
    mkdir -p logs
    mv log_$current_time.log.gz logs/
}
function ctrlc_exit () {
    archive_logs
    exit 0
}
trap "ctrlc_exit" 2
for (( ; ; ))
do
    set -o pipefail
    current_time=$(date +"%Y%m%d%H%M")
    ./BallanceMMOServer "$@" 2>&1 | tee log_$current_time.log
    server_status=$?
    archive_logs
    if [ $server_status -eq 0 ] || [ $server_status -eq 1 ]
    then
        break
    fi
done
