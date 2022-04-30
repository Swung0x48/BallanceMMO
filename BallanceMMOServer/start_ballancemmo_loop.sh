#! /bin/bash
function archive_logs () {
    gzip -v $log_name
    mkdir -p logs
    mv $log_name.gz logs/
}
function ctrlc_exit () {
    archive_logs
    exit 0
}
trap "ctrlc_exit" 2
for (( ; ; ))
do
    set -o pipefail
    log_name=$(date +"log_%Y%m%d%H%M.log")
    ./BallanceMMOServer "$@" 2>&1 | tee $log_name
    server_status=$?
    archive_logs
    if [ $server_status -eq 0 ] || [ $server_status -eq 1 ]
    then
        break
    fi
done
