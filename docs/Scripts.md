# Transmission scripts
## Introduction
Thanks to the powerful [RPC](./rpc-spec.md), `transmission-remote` can talk to any client that has the RPC enabled. This means that a script written using `transmission-remote` or [RPC](./rpc-spec.md) can, without rewrite, communicate with all the Transmission clients: Mac, Linux, Windows, and headless.

macOS users may wonder whether there will be AppleScript scripts, the answer is ''no''. Although AppleScript is a nice technology, it's a pain to implement. However, macOS is a Unix after all, so any script you find here will also work on macOS. Even from within AppleScript, you can run these scripts by typing: `do shell script "path/to/script"`.

## How-To
If you are interested at writing scripts for Transmission, have a look at the following pages:
 * [Configuration Files](Configuration-Files.md)
 * [Editing Configuration Files](Editing-Configuration-Files.md)
 * [Environment Variables](Environment-Variables.md)
 * [RPC Protocol Specification](rpc-spec.md)

For those who need more information how to use the scripts, have a look at the following links:
 * [Cron How-To](https://help.ubuntu.com/community/CronHowto ): Run scripts at a regular interval

## Scripts

### Nashawk Usenet Web UI test daemon
For local Usenet/Web UI field testing, use:

```sh
./scripts/run-usenet-webui-test.sh
```

The script loads local Usenet settings from `.env`, selects Node.js `24.18.0`
through nvm when available, points `TRANSMISSION_WEB_HOME` at
`web/public_html`, and starts the built daemon in the foreground. Press
`Ctrl-C` to stop it.

The Web UI is served at:

```text
http://127.0.0.1:19091/transmission/web/
```

Default test paths and settings:

```text
RPC: http://127.0.0.1:19091/transmission/web/
Config: /tmp/nashawk-webui-real/config
Downloads: /tmp/nashawk-webui-real/downloads
Incomplete: /tmp/nashawk-webui-real/incomplete
Log: /tmp/nashawk-webui-real/daemon.log
Usenet article size check: 2097152 bytes
Usenet upload concurrency: 40
Usenet discovery: enabled
Usenet discovery sample size: 16
Usenet eviction: enabled
Usenet cache size: 0 MiB
```

Common overrides:

```sh
NASHAWK_RPC_PORT=19092 ./scripts/run-usenet-webui-test.sh
NASHAWK_LOG_FILE=/tmp/nashawk-webui-real/daemon-debug.log ./scripts/run-usenet-webui-test.sh
NASHAWK_USENET_CHECK_ARTICLE_SIZE=8388608 ./scripts/run-usenet-webui-test.sh
NASHAWK_USENET_EVICTION_MIN_AGE_MINUTES=5 ./scripts/run-usenet-webui-test.sh
NASHAWK_USENET_DISCOVERY_ENABLED=0 ./scripts/run-usenet-webui-test.sh
NASHAWK_USENET_DISCOVERY_SAMPLE_SIZE=8 ./scripts/run-usenet-webui-test.sh
```

### On torrent completion
Transmission can be set to invoke a script when downloads complete. The environment variables supported are:

 * `TR_APP_VERSION` - Transmission's short version string, e.g. `4.0.0`
 * `TR_TIME_LOCALTIME`
 * `TR_TORRENT_BYTES_DOWNLOADED` - Number of bytes that were downloaded for this torrent
 * `TR_TORRENT_DIR` - Location of the downloaded data
 * `TR_TORRENT_HASH` - The torrent's info hash
 * `TR_TORRENT_ID`
 * `TR_TORRENT_LABELS` - A comma-delimited list of the torrent's labels
 * `TR_TORRENT_NAME` - Name of torrent (not filename)
 * `TR_TORRENT_PRIORITY` - The priority of the torrent (Low is "-1", Normal is "0", High is "1")
 * `TR_TORRENT_TRACKERS` - A comma-delimited list of the torrent's trackers' announce URLs

[Here is an example script](https://github.com/transmission/transmission/blob/main/extras/send-email-when-torrent-done.sh) that sends an email when a torrent finishes.

### Obsolete
Functionality of these scripts has been implemented in libtransmission and is thus available in all clients.

 * [Email Notification Script](https://github.com/transmission/transmission/blob/main/extras/send-email-when-torrent-done.sh)

## contrib/scripts
Tomas Carnecky (aka wereHamster) is maintaining a set of scripts in his [GitHub repository](https://github.com/wereHamster/transmission/tree/master/contrib/scripts/ ).

Oguz wrote [on his blog](https://oguzarduc.blogspot.com/2012/05/transmission-quit-script-in-php.html) a PHP script to stop Transmission after it finishes downloading and seeding.
Scripts which have not yet been ported and may not work with the latest version:
 * https://pastebin.com/QzVxQDtM: Bash - (cron)script to keep a maximum number of torrents running; starting and pausing torrents as necessary
 * https://github.com/jaboto/Transmission-script - (cron)script set network limits according to the number of clients in the network

## Security with systemd
`transmission-daemon`'s packaging has many permissions disabled as a standard safety measure. If your script needs more permissions than are provided by the default, users have [reported](https://github.com/transmission/transmission/issues/1951) that it can be resolved by changing to `NoNewPrivileges=false` using a systemd unit override.

```
$ sudo systemctl edit transmission-daemon.service
```

and add the following content to the override:

```
[Service]
NoNewPrivileges=false
```

and that override will be kept untouched by package upgrades.
