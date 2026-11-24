# Local Files API fixture: python tests/files_fixture.py
# Uses loopback and temporary download files; no account or external server is needed.
import http.server,threading,json,hashlib,urllib.parse,subprocess,os,random
from pathlib import Path
payload=bytes(range(251))*4096
errors=[]
logins=0
game_updated=False
updated_bytes=0
full_updates=0
manifest_drift=False
original_game=random.Random(73).randbytes(12*1024*1024)
changed_game=original_game[:4000]+b'inserted game content'+original_game[4000:]

def checksum(data):
 return hashlib.sha256(data).hexdigest()

def file_entry(path,data):
 return {'path':path,'directory':False,'size':len(data),'sha256':checksum(data),'last_modified':None}

def game_chunks(data):
 mask=(1<<64)-1
 def gear(byte):
  value=(byte+0x9e3779b97f4a7c15)&mask
  value=((value^(value>>30))*0xbf58476d1ce4e5b9)&mask
  value=((value^(value>>27))*0x94d049bb133111eb)&mask
  return value^(value>>31)
 table=[gear(byte) for byte in range(256)]
 result=[]
 start=rolling=0
 for end,byte in enumerate(data,1):
  rolling=((rolling<<1)+table[byte])&mask
  size=end-start
  if size==4*1024*1024 or (size>=256*1024 and not rolling&((1<<20)-1)):
   result.append({'offset':start,'size':size,'sha256':checksum(data[start:end])})
   start,rolling=end,0
 if start!=len(data): result.append({'offset':start,'size':len(data)-start,'sha256':checksum(data[start:])})
 return result
changed_chunks=game_chunks(changed_game)

class Handler(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args): pass
 def send_json(self,status,value):
  data=json.dumps(value).encode()
  self.send_response(status);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
 def do_POST(self):
  global logins
  logins+=1
  data=json.loads(self.rfile.read(int(self.headers.get('Content-Length',0))))
  if self.path!='/auth/v1/login' or data.get('username')!='reader': errors.append('Unexpected login')
  self.send_json(200,{'token':'download-token','device':{'id':'desktop'},'user':{'username':'reader','display_name':'Reader'},'access':{'sync':True}})
 def do_DELETE(self):
  global game_updated
  if self.path=="/auth/v1/device": game_updated=True
  self.send_json(200,{'ok':True})
 def do_GET(self):
  global updated_bytes,full_updates,manifest_drift
  url=urllib.parse.urlsplit(self.path); query=urllib.parse.parse_qs(url.query,keep_blank_values=True);path=query.get('path',[''])[0]
  if url.path=='/legacy/files/v1': self.send_json(200,'Legacy LOKI page');return
  if url.path=='/files/v1': self.send_json(200,{'protocol':1,'checksum':'sha256','name':'Files test'}); return
  if self.headers.get('Authorization')!='Bearer download-token': self.send_json(401,{'error':'Sign in again.','code':'unauthorized'});return
  if path=='forbidden': self.send_json(403,{'error':'Access revoked.','code':'forbidden'});return
  if url.path=='/auth/v1/me': self.send_json(200,{'access':{'sync':True}});return
  if url.path=='/files/v1/shares': self.send_json(200,{'shares':[{'id':'archive','name':'Archive','supports_updates':True}]});return
  game=changed_game if game_updated else original_game
  game_files={'assets/keep.txt':b'unchanged','game.exe':game}
  game_files.update({'new.txt':b'added'} if game_updated else {'old.txt':b'remove'})
  if url.path.endswith('/manifest'):
   directory=path=='Game'
   if directory:
    folder=lambda name: {'path':name,'directory':True,'size':0,'sha256':None,'last_modified':None}
    entries=[folder(name) for name in ['assets','assets/empty']]
    entries.extend(file_entry(name,data) for name,data in sorted(game_files.items()))
   elif path.startswith('Game/') and path.removeprefix('Game/') in game_files:
    entries=[file_entry('',game_files[path.removeprefix('Game/')])]
   else:
    self.send_json(404,{'error':'Missing'});return
   revision=checksum(json.dumps(entries).encode())
   if directory and manifest_drift:
    manifest_drift=False
    revision=checksum(b'game changed during transfer')
   self.send_json(200,{'directory':directory,'revision':revision,'entries':entries});return
  if url.path.endswith('/chunks'):
   if path!='Game/game.exe' or not game_updated or query.get('sha256')!=[checksum(game)]:
    errors.append('Unexpected chunks request')
   self.send_json(200,{'chunks':changed_chunks,'next_offset':None});return
  if url.path.endswith('/entries'):
   if path!='Games/日本語 + #' or query.get('page')!=['1']: errors.append('Path or page encoding failed: '+path)
   self.send_json(200,{'entries':[{'name':'日本語 + #.bin','directory':False,'size':len(payload),'modified':1793836800}],'total':101,'page':1});return
  data=game_files.get(path.removeprefix('Game/'),b'') if path.startswith('Game/') else original_game if path=='baseline' else b'' if path=='empty' else payload
  if url.path.endswith('/metadata'):
   self.send_json(200,{'size':len(data),'sha256':'0'*64 if path=='badHash' else checksum(data),'last_modified':'Thu, 05 Nov 2026 00:00:00 GMT'});return
  if url.path.endswith('/file'):
   request_range=self.headers.get('Range')
   start,end=request_range.removeprefix('bytes=').split('-') if request_range else ('0','')
   offset=int(start)
   end=int(end) if end else len(data)-1
   if request_range and not end<len(data): errors.append('Range exceeds file length')
   if request_range and request_range.endswith('-') and self.headers.get('If-Range')!='Thu, 05 Nov 2026 00:00:00 GMT': errors.append('Missing resume validator')
   if path=='restart': request_range=None;offset=0
   self.send_response(206 if request_range else 200)
   if request_range:self.send_header('Content-Range',f'bytes {offset+1 if path=="badRange" else offset}-{end}/{len(data)}')
   self.send_header('Content-Length',str(end+1-offset));self.end_headers()
   if game_updated and path=='Game/game.exe':
    if request_range:
     if updated_bytes==0: manifest_drift=True
     updated_bytes+=end+1-offset
    else: full_updates+=1
   try:
    self.wfile.write(data[offset:offset+65536] if path=='interrupt' else data[offset:end+1]);self.wfile.flush()
   except (BrokenPipeError,ConnectionResetError,ConnectionAbortedError):pass
   self.close_connection=True
   return
  self.send_json(404,{'error':'Missing'})
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
threading.Thread(target=server.serve_forever,daemon=True).start()
env=os.environ.copy();env['LOKI_FILES_TEST_SERVER']=f'http://127.0.0.1:{server.server_port}'
try:
 build=subprocess.run(['xmake','build','loki-sync-tests'],cwd=Path(__file__).resolve().parent.parent,env=env)
 if build.returncode:raise SystemExit(build.returncode)
 run=subprocess.run(['xmake','run','loki-sync-tests'],cwd=Path(__file__).resolve().parent.parent,env=env)
 if logins!=1: errors.append(f'Expected one login for sync and files, got {logins}')
 if run.returncode==0 and not (0<updated_bytes<len(changed_game) and full_updates==1): errors.append(f'Expected a delta and one full redownload: {updated_bytes} delta bytes, {full_updates} full downloads')
 if errors: print(errors);raise SystemExit(1)
 if run.returncode==0: print(f'Binary update ranges sent {updated_bytes:,} bytes across the interrupted update and retry, for a {len(changed_game):,}-byte file.')
 raise SystemExit(run.returncode)
finally: server.shutdown()
