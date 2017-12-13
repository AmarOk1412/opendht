import requests
import sys
import random
import threading
import multiprocessing

baseHash = 0
listeners = 8000
nbThreads = 240
proxyput = 'http://ring-prod-mtl-02:8000/'
proxylisten = 'http://ring-prod-mtl-01:8000/'
# proxylisten = 'http://ubuntu-qemu.local:8000/'

def worker():
    while True:
        i = random.randint(baseHash, baseHash + listeners)
        print(f'PUT ON /{i}')
        try:
            requests.request('POST', f'{proxyput}{i}',
                             json={"data": "YQ==", "id": "0", "type": 0})
        except:
            pass


if len(sys.argv) == 3 and sys.argv[1] == 'c':
    for i in range(baseHash, baseHash + listeners):
        print(f'SUBSCRIBE /{i}')
        deviceKey = sys.argv[2]  #if (i % 10 == 0) else 'not_a_key_to_avoid_to_crash_my_android'
        req = requests.request('SUBSCRIBE', f'{proxylisten}{i}', json={"key": deviceKey})
elif len(sys.argv) == 2 and sys.argv[1] == 'd':
    for i in range(baseHash, baseHash + listeners):
        print(f'UNSUBSCRIBE /{i}')
        deviceKey = sys.argv[2]  if (i % 100 == 0) else 'not_a_key_to_avoid_to_crash_my_android'
        req = requests.request('UNSUBSCRIBE', f'{proxylisten}{i}', json={"key": deviceKey})
else:
    for i in range (0, nbThreads):
        t = threading.Thread(target=worker)
        t.start()
