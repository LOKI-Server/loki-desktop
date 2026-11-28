# Local Files API fixture: python tests/files_fixture.py
# Uses loopback and temporary download files; no account or external server is needed.
import http.server,threading,json,hashlib,urllib.parse,subprocess,os,random
from pathlib import Path
payload=bytes(range(251))*4096
errors=[]
logins=0
game=random.Random(73).randbytes(3*1024*1024)
game_files={'assets/keep.txt':b'unchanged','game.exe':game}

def checksum(data):
 return hashlib.sha256(data).hexdigest()

def file_entry(path,data):
 return {'path':path,'directory':False,'size':len(data),'sha256':checksum(data),'last_modified':None}

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
  self.send_json(200,{'ok':True})
 def do_GET(self):
  url=urllib.parse.urlsplit(self.path); query=urllib.parse.parse_qs(url.query,keep_blank_values=True);path=query.get('path',[''])[0]
  if url.path=='/legacy/files/v1': self.send_json(200,'Legacy LOKI page');return
  if url.path=='/files/v1': self.send_json(200,{'protocol':1,'checksum':'sha256','name':'Files test'}); return
  if self.headers.get('Authorization')!='Bearer download-token': self.send_json(401,{'error':'Sign in again.','code':'unauthorized'});return
  if path=='forbidden': self.send_json(403,{'error':'Access revoked.','code':'forbidden'});return
  if url.path=='/auth/v1/me': self.send_json(200,{'access':{'sync':True}});return
  if url.path=='/files/v1/shares': self.send_json(200,{'shares':[{'id':'archive','name':'Archive'}]});return
  if url.path.endswith('/manifest'):
   # "Drift" changes between the two manifest reads of one download.
   if path not in ('Game','Drift'): self.send_json(400,{'error':'Manifests describe folders.'});return
   folder=lambda name: {'path':name,'directory':True,'size':0,'sha256':None,'last_modified':None}
   entries=[folder(name) for name in ['assets','assets/empty']]
   entries.extend(file_entry(name,data) for name,data in sorted(game_files.items()))
   revision=checksum(json.dumps(entries).encode()+(os.urandom(8) if path=='Drift' else b''))
   self.send_json(200,{'directory':True,'revision':revision,'entries':entries});return
  if url.path.endswith('/entries'):
   if path!='Games/日本語 + #' or query.get('page')!=['1']: errors.append('Path or page encoding failed: '+path)
   self.send_json(200,{'entries':[{'name':'日本語 + #.bin','directory':False,'size':len(payload),'modified':1793836800}],'total':101,'page':1});return
  member=path.split('/',1)[1] if path.startswith(('Game/','Drift/')) else None
  data=game_files.get(member,b'') if member is not None else b'' if path=='empty' else payload
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
 if errors: print(errors);raise SystemExit(1)
 raise SystemExit(run.returncode)
finally: server.shutdown()
