ports={0x1E32:'7730(default)',0x1E33:'7731(env=0.0.0.0)'}
for line in open('/proc/net/tcp').read().splitlines()[1:]:
    f=line.split(); ip,po=f[1].split(':'); po=int(po,16)
    if po in ports and f[3]=='0A':  # 0A = LISTEN
        b=bytes.fromhex(ip); addr='.'.join(str(x) for x in b[::-1])
        print(ports[po],'-> bind',addr)
