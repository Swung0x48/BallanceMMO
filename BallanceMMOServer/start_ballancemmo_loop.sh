#! /bin/bash
function archive_logs () {
    gzip -v $log_name
    mkdir -p logs
    mv $log_name.gz logs/
}
function cleanup () {
    archive_logs
    if stty "$original_settings" 2>/dev/null; then
        stty sane 2>/dev/null # Hard reset if not able to restore
    fi
    exit 0
}
trap "cleanup" SIGINT SIGTERM SIGHUP

original_settings=$(stty -g)
for (( ; ; ))
do
    log_name=$(date +"log_%Y%m%d%H%M.log")
    server_binary="./BallanceMMOServer"
    extra_options=()
    if [ ! -f "$server_binary" ]; then
        server_binary=$(ls ./BallanceMMOLaunchSelector*.AppImage | awk '{print $1}')
        if [ ! -f "$server_binary" ]; then
            echo "Error: server executable not found." >&2
            exit 1
        fi
        extra_options=("--launch" "Server")
    fi
    chmod +x $server_binary
    SECONDS=0
    $server_binary "${extra_options[@]}" --log=$log_name "$@"
    server_status=$?
    archive_logs
    if [ $server_status -eq 0 ] || [ $server_status -eq 1 ] || [ $SECONDS -lt 10 ]
    then
        break
    fi
done
