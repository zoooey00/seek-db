"""Deterministic local test double; not the competition's official Mock service."""
import argparse
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

REQUESTS = []
LOCK = threading.Lock()

def embedding(text, dimension):
    seed = sum(str(text).encode('utf-8')) % 997
    return [float(seed + i) for i in range(dimension)]

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def send(self, value, status=200):
        data = json.dumps(value, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        with LOCK:
            self.send({'requests': list(REQUESTS)})

    def do_DELETE(self):
        with LOCK:
            REQUESTS.clear()
        self.send({'ok': True})

    def do_POST(self):
        try:
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            with LOCK:
                REQUESTS.append({'path': self.path, 'body': body,
                                 'authorization': bool(self.headers.get('Authorization'))})
            if 'wrong-root' in self.path:
                self.send([])
            elif 'http-error' in self.path:
                self.send({'error': 'deliberate nonretryable failure'}, 400)
            elif 'fail' in self.path:
                self.send({'error': 'deliberate test failure'}, 503)
            elif self.path.endswith('/api/generate'):
                if body.get('stream') is not False or 'prompt' not in body:
                    self.send({'error': 'native non-streaming protocol required'}, 400)
                else:
                    self.send({'response': body['prompt'], 'done': True})
            elif self.path.endswith('/api/embed'):
                inputs = body['input']
                if not isinstance(inputs, list):
                    raise ValueError('input must be an array')
                dimension = body.get('dimensions', self.server.embedding_dimension)
                self.send({'embeddings': [embedding(x, dimension) for x in inputs]})
            elif 'embeddings' in self.path:
                dimension = body.get('dimensions', self.server.embedding_dimension)
                data = [{'index': i, 'embedding': embedding(x, dimension)} for i, x in enumerate(body['input'])]
                if 'bad-dim' in self.path:
                    data[0]['embedding'].append(1.0)
                if 'bad-index' in self.path and len(data) > 1:
                    data[-1]['index'] = data[0]['index']
                if 'bad-vector' in self.path:
                    data[0]['embedding'] = ['invalid']
                # The engine must reorder by index, never by response position.
                self.send({'data': list(reversed(data))})
            elif 'rerank' in self.path:
                data = [{'index': i, 'relevance_score': (sum(x.encode()) % 7) / 10.0}
                        for i, x in enumerate(body['documents'])]
                if 'bad-index' in self.path:
                    data[-1]['index'] = -1
                self.send({'results': list(reversed(data))})
            elif 'chat/completions' in self.path:
                value = body['messages'][-1]['content']
                if 'empty' in self.path:
                    value = ''
                self.send({'choices': [{'message': {'content': value}}]})
            else:
                self.send({'error': 'unknown test route'}, 404)
        except (ValueError, KeyError, TypeError) as exc:
            self.send({'error': str(exc)}, 400)

def start(host='127.0.0.1', port=18080, dimension=3):
    service = ThreadingHTTPServer((host, port), Handler)
    service.embedding_dimension = dimension
    threading.Thread(target=service.serve_forever, daemon=True).start()
    return service

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=8000)
    parser.add_argument('--dimension', type=int, default=3)
    args = parser.parse_args()
    service = ThreadingHTTPServer((args.host, args.port), Handler)
    service.embedding_dimension = args.dimension
    service.serve_forever()
