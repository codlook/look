#!/bin/bash
# LOOK Language — ortam dosyasini yukle, sonra LOOK'u baslat
# Kullanim: look-env <env-file> <command> [args...]
#
# enable.sh bunu /opt/look/look-env olarak kurar ve unit'te ExecStart'in basina koyar.
# Servisin kullanicisi (User=) olarak calisir: dosyayi o kullanicinin haklariyla okur —
# baska sitenin dosyasina giden symlink "Permission denied" ile durur.
# Bicim systemd EnvironmentFile ile ayni: KEY=VALUE satirlari, '#' ile baslayan satir yorum,
# deger cevresindeki "..." / '...' kaldirilir. Satir icinde yorum YOKTUR: "KEY=a # b"
# degeri "a # b"dir — bu sik bir tuzak oldugu icin journal'a uyari yazilir.

ENV_FILE="$1"; shift
if ! exec 3< "$ENV_FILE"; then
    echo "LOOK: cannot read the env file $ENV_FILE as $(id -un)" >&2
    exit 1
fi
n=0
while IFS= read -r line <&3 || [ -n "$line" ]; do
    n=$((n + 1))
    line="${line%$'\r'}"
    trimmed="${line#"${line%%[![:space:]]*}"}"
    case "$trimmed" in ''|'#'*|';'*) continue ;; esac
    case "$trimmed" in export[[:space:]]*) trimmed="${trimmed#export}"; trimmed="${trimmed#"${trimmed%%[![:space:]]*}"}" ;; esac
    case "$trimmed" in *=*) ;; *) echo "LOOK: $ENV_FILE line $n has no '=', ignored" >&2; continue ;; esac
    key="${trimmed%%=*}"; key="${key%"${key##*[![:space:]]}"}"
    val="${trimmed#*=}"
    val="${val#"${val%%[![:space:]]*}"}"; val="${val%"${val##*[![:space:]]}"}"
    if ! [[ "$key" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]]; then
        echo "LOOK: $ENV_FILE line $n: '$key' is not a variable name, ignored" >&2; continue
    fi
    if [ "${#val}" -ge 2 ] && { [[ "$val" == \"*\" ]] || [[ "$val" == \'*\' ]]; }; then
        val="${val:1:${#val}-2}"
    elif [[ "$val" == *[[:space:]]#* ]]; then
        echo "LOOK: $ENV_FILE line $n: the value of $key contains ' #' — inline comments are not supported, the text after # is part of the value" >&2
    fi
    export "$key=$val"
done
exec 3<&-
exec "$@"
