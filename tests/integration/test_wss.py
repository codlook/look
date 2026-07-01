import asyncio, json, websockets, ssl

async def test():
    url = "wss://looktest.tobiyo.com.tr/chat"
    ssl_ctx = ssl.create_default_context()
    ssl_ctx.check_hostname = False
    ssl_ctx.verify_mode = ssl.CERT_NONE
    print("Baglaniyor:", url)
    try:
        async with websockets.connect(url, ssl=ssl_ctx, ping_interval=None) as ws:
            print("Baglandi!")
            await ws.send(json.dumps({"isim": "test", "metin": "merhaba nginx"}))
            msg = await asyncio.wait_for(ws.recv(), timeout=5)
            data = json.loads(msg)
            print("Alindi:", data)
            if data.get("tip") == "mesaj":
                print("WSS /chat uzerinden PASS")
    except Exception as e:
        print("HATA:", e)

asyncio.run(test())
