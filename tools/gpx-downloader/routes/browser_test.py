"""Chrome integration tests. Install Playwright and its Chromium browser first."""
import functools
import http.server
import os
from pathlib import Path
import threading
import unittest
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[3]

class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass

class BrowserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(QuietHandler, directory=str(ROOT)))
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.playwright = sync_playwright().start()
        options = {'headless': True, 'args': ['--no-sandbox']}
        if os.environ.get('CHROMIUM_PATH'):
            options['executable_path'] = os.environ['CHROMIUM_PATH']
        cls.browser = cls.playwright.chromium.launch(**options)
        cls.url = f'http://127.0.0.1:{cls.server.server_port}/tools/gpx-downloader/'

    @classmethod
    def tearDownClass(cls):
        cls.browser.close()
        cls.playwright.stop()
        cls.server.shutdown()

    def setUp(self):
        self.page = self.browser.new_page()
        self.errors = []
        self.page.on('pageerror', lambda e: self.errors.append(str(e)))
        self.page.goto(self.url)
        self.page.locator('#route-import-tab').click()

    def tearDown(self):
        self.assertEqual([], self.errors)
        self.page.close()

    def test_namespaces_segments_missing_heights_and_invalid_coordinates(self):
        results = self.page.evaluate('''async () => {
          const {parseGpxDocument,convert}=await import('./routes/format.mjs');
          const parse=xml=>parseGpxDocument(new DOMParser().parseFromString(xml,'application/xml'));
          let checks=0;
          for(const version of ['1.0','1.1']) {
            const ns=`http://www.topografix.com/GPX/${version.replace('.','/')}`;
            const gpx=parse(`<g:gpx xmlns:g="${ns}" version="${version}"><g:wpt lat="1" lon="2"><g:name>水</g:name></g:wpt><g:trk><g:name>Hike</g:name><g:trkseg><g:trkpt lat="1" lon="2"><g:ele>100</g:ele></g:trkpt><g:trkpt lat="1.001" lon="2"/></g:trkseg><g:trkseg><g:trkpt lat="2" lon="2"/><g:trkpt lat="2.001" lon="2"/></g:trkseg></g:trk><g:rte><g:rtept lat="1" lon="2"/><g:rtept lat="1.001" lon="2"/></g:rte></g:gpx>`);
            const route=convert(gpx.paths[0],gpx.waypoints);
            if(gpx.paths.length!==2||route.segments!==2||route.points[1].alt!==null||route.checkpoints[0].name!=='水'||route.distance>300)throw Error('GPX mismatch');checks++;
          }
          for(const point of ['lat="" lon="0"','lat="91" lon="0"','lat="NaN" lon="0"','lon="0"']) {
            let rejected=false;try{parse(`<gpx xmlns="http://www.topografix.com/GPX/1/1" version="1.1"><trk><trkseg><trkpt ${point}/><trkpt lat="0" lon="0"/></trkseg></trk></gpx>`);}catch{rejected=true;}if(!rejected)throw Error('Invalid coordinate accepted');checks++;
          }
          for(const xml of ['<gpx>','<gpx version="1.1"><trk/></gpx>']){let rejected=false;try{parse(xml);}catch{rejected=true;}if(!rejected)throw Error('Bad XML accepted');checks++;}
          return checks;
        }''')
        self.assertEqual(8, results)

    def test_worker_preview_and_usb_import_above_32kb(self):
        self.page.evaluate('''() => {
          const serial={requestPort:async()=>{
            let controller,offset=0,state=2,session=0;
            const readable=new ReadableStream({start(c){controller=c;}});
            const writable=new WritableStream({write(frame){
              const req=frame.subarray(3),v=new DataView(req.buffer,req.byteOffset,req.length),op=req[4];let body=new Uint8Array();
              if(op===1){body=new Uint8Array(22);const d=new DataView(body.buffer);d.setUint16(0,144,true);d.setUint32(2,1048576,true);d.setUint32(10,65536,true);d.setUint32(18,900000,true);}
              if(op===3){state=3;session=77;offset=0;window.uploadBytes=v.getUint32(11,true);}
              if(op===4){if(v.getUint32(11,true)!==offset)throw Error('Bad offset');offset+=req.length-15;}
              if(op===6){state=1;window.importComplete=true;}
              if(op===5){body=new Uint8Array(5);new DataView(body.buffer).setUint32(0,100,true);}
              const reply=new Uint8Array(17+body.length),d=new DataView(reply.buffer);reply.set([127,83,82,1,op,req[5],req[6]]);d.setUint32(7,session,true);reply[12]=state;d.setUint32(13,offset,true);reply.set(body,17);
              const wire=new Uint8Array(3+reply.length);wire.set([62,reply.length,0]);wire.set(reply,3);controller.enqueue(wire.subarray(0,8));controller.enqueue(wire.subarray(8));
            }});
            return {readable,writable,open:async(o)=>{if(o.baudRate!==115200)throw Error('Wrong baud');},close:async()=>{window.serialClosed=true;}};
          }};Object.defineProperty(navigator,'serial',{value:serial});
        }''')
        points = ''.join(f'<trkpt lat="{i*.0001}" lon="0"><ele>{100+i/100}</ele></trkpt>' for i in range(3000))
        xml = f'<gpx xmlns="http://www.topografix.com/GPX/1/1" version="1.1"><wpt lat="0" lon="0"><name>Water 水</name></wpt><trk><name>Long hike</name><trkseg>{points}</trkseg></trk></gpx>'
        self.page.locator('#route-file').set_input_files({'name': 'large.gpx', 'mimeType': 'application/gpx+xml', 'buffer': xml.encode()})
        self.page.wait_for_function("document.querySelector('#route-summary').textContent.includes('3000 / 3000')")
        self.page.locator('#route-connect').click()
        self.page.wait_for_function("!document.querySelector('#route-upload').disabled")
        self.page.locator('#route-upload').click()
        self.page.wait_for_function("document.querySelector('#route-status').textContent.includes('imported and verified')")
        self.assertGreater(self.page.evaluate('window.uploadBytes'), 32768)
        self.assertTrue(self.page.evaluate('window.importComplete'))
        self.page.locator('#route-export-tab').click()
        self.assertTrue(self.page.evaluate('window.serialClosed'))
        self.assertTrue(self.page.locator('#route-import').is_hidden())

if __name__ == '__main__':
    unittest.main()
