import http.server, socketserver, os, sys, threading
D = os.path.dirname(os.path.abspath(__file__))
class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self,*a,**k): super().__init__(*a,directory=D,**k)
    def do_POST(self):
        n = int(self.headers.get('Content-Length','0'))
        body = self.rfile.read(n).decode('utf-8','replace')
        open(os.path.join(D,'result.txt'),'a').write(body+'\n==== end ====\n')
        self.send_response(204); self.end_headers()
        sys.stderr.write('GOT RESULT\n'); sys.stderr.flush()
    def log_message(self,*a): pass
socketserver.TCPServer.allow_reuse_address = True
with socketserver.TCPServer(('127.0.0.1', 8731), H) as s:
    s.serve_forever()
