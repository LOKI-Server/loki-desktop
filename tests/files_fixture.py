# Local Files API fixture: python tests/files_fixture.py
# Uses loopback and temporary download files; no account or external server is needed.
import http.server,threading,json,hashlib,urllib.parse,subprocess,os
from pathlib import Path
payload=bytes(range(251))*4096
errors=[]
logins=0
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
 def do_DELETE(self): self.send_json(200,{'ok':True})
 def do_GET(self):
  url=urllib.parse.urlsplit(self.path); query=urllib.parse.parse_qs(url.query,keep_blank_values=True);path=query.get('path',[''])[0]
  if url.path=='/legacy/files/v1': self.send_json(200,'Legacy LOKI page');return
  if url.path=='/files/v1': self.send_json(200,{'protocol':1,'checksum':'sha256','name':'Files test'}); return
  if self.headers.get('Authorization')!='Bearer download-token': self.send_json(401,{'error':'Sign in again.','code':'unauthorized'});return
  if path=='forbidden': self.send_json(403,{'error':'Access revoked.','code':'forbidden'});return
  if url.path=='/auth/v1/me': self.send_json(200,{'access':{'sync':True}});return
  if url.path=='/files/v1/shares': self.send_json(200,{'shares':[{'id':'archive','name':'Archive'}]});return
  if url.path.endswith('/entries'):
   if path!='Games/日本語 + #' or query.get('page')!=['1']: errors.append('Path or page encoding failed: '+path)
   self.send_json(200,{'entries':[{'name':'日本語 + #.bin','directory':False,'size':len(payload),'modified':1793836800}],'total':101,'page':1});return
  data=b'' if path=='empty' else payload
  if url.path.endswith('/metadata'):
   self.send_json(200,{'size':len(data),'sha256':'0'*64 if path=='badHash' else hashlib.sha256(data).hexdigest(),'last_modified':'Thu, 05 Nov 2026 00:00:00 GMT'});return
  if url.path.endswith('/file'):
   header=self.headers.get('Range');offset=int(header.removeprefix('bytes=').removesuffix('-')) if header else 0
   if header and self.headers.get('If-Range')!='Thu, 05 Nov 2026 00:00:00 GMT': errors.append('Missing resume validator')
   if path=='restart': offset=0
   self.send_response(206 if offset else 200)
   if offset:self.send_header('Content-Range',f'bytes {offset+1 if path=="badRange" else offset}-{len(data)-1}/{len(data)}')
   self.send_header('Content-Length',str(len(data)-offset));self.end_headers()
   try:
    self.wfile.write(data[offset:offset+65536] if path=='interrupt' else data[offset:]);self.wfile.flush()
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
