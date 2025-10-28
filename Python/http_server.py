"""
HTTP server module - corresponds to SimpleHttpServer in C#
Fully implements the logic of the C# version
"""
import json
import threading
import socket
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs
from typing import Dict, Any, List
from data_structures import SettingParameters, ROI, ROIGroup
import asyncio

class SimpleHttpHandler(BaseHTTPRequestHandler):
    """HTTP request handler - corresponds to C# HttpListener"""
    
    server_instance = None  # Class variable to store server instance
    
    def log_message(self, format, *args):
        """Disable log output"""
        pass
    
    def do_POST(self):
        """Handle POST requests"""
        if self.path == "/SetParameters":
            self._handle_set_parameters()
        else:
            self._send_not_found()
    
    def do_GET(self):
        """Handle GET requests"""
        if self.path == "/Alive":
            self._handle_alive()
        elif self.path == "/GetLicense":
            self._handle_get_license()
        else:
            self._send_not_found()
    
    def _initialize_rois_array(self, size: int) -> List[ROI]:
        """Initialize ROI array - corresponds to C# InitializeRoisArray"""
        rois = []
        for i in range(size):
            rois.append(ROI(x=-1, y=-1))  # Default values
        return rois
    
    def _handle_set_parameters(self):
        """Handle set parameters request - corresponds to C# SetParameters logic"""
        try:
            # Read request body - corresponds to C# StreamReader
            content_length = int(self.headers.get('Content-Length', 0))
            request_body = self.rfile.read(content_length).decode('utf-8')
            
            print(f"Received SetParameters request: {request_body}")
            
            # Parse JSON - corresponds to C# JsonConvert.DeserializeObject
            json_data = json.loads(request_body)
            
            # Initialize SettingParameters structure - corresponds to C# SettingParameters settings
            settings = SettingParameters()
            settings.version = json_data.get("version", "1.2")
            settings.analytics_event_api_url = json_data.get("analytics_event_api_url", "")
            settings.image_width = int(json_data.get("image_width", 0))
            settings.image_height = int(json_data.get("image_height", 0))
            settings.jpg_compress = int(json_data.get("jpg_compress", 50))
            
            # Parse rois array
            json_rois = json_data.get("rois", [])
            settings.rois = []
            
            for roi_group_data in json_rois:
                roi_group = ROIGroup()
                roi_group.sensitivity = int(roi_group_data.get("sensitivity", 50))
                roi_group.threshold = int(roi_group_data.get("threshold", 50))
                
                # Parse rects array (should have 4 corner points)
                rects_data = roi_group_data.get("rects", [])
                roi_group.rects = []
                for rect_point in rects_data:
                    roi_point = ROI(
                        x=int(rect_point.get("x", -1)),
                        y=int(rect_point.get("y", -1))
                    )
                    roi_group.rects.append(roi_point)
                
                settings.rois.append(roi_group)
            
            print(f"Parsed {len(settings.rois)} ROI groups:")
            for i, roi_group in enumerate(settings.rois):
                print(f"  ROI Group {i}: sensitivity={roi_group.sensitivity}, threshold={roi_group.threshold}, {len(roi_group.rects)} points")
                for j, point in enumerate(roi_group.rects):
                    print(f"    Point {j}: x={point.x}, y={point.y}")
            
            # Set to server instance - corresponds to C# _parameters = settings
            # Ensure _updateparams is set to True
            if self.server_instance:
                self.server_instance._parameters = settings
                self.server_instance._updateparams = True
            else:
                print("[ERROR] Server instance is not set. Unable to update parameters.")
            
            # Send response - corresponds to C# response logic
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            
            response_data = {"message": "Parameters set successfully"}
            response_string = json.dumps(response_data)
            self.wfile.write(response_string.encode('utf-8'))
            
        except Exception as e:
            print(f"Error setting parameters: {e}")
            import traceback
            traceback.print_exc()
            self.send_response(400)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            error_response = {"error": str(e)}
            self.wfile.write(json.dumps(error_response).encode('utf-8'))
    
    def _handle_alive(self):
        """Handle alive check request - corresponds to C# /Alive"""
        self.send_response(200)
        self.send_header('Content-Type', 'text/plain')
        self.end_headers()
        self.wfile.write(b"")
    
    def _handle_get_license(self):
        """Handle license check request - corresponds to C# /GetLicense"""
        # should add code to check license is exist.
        self.send_response(200)
        self.send_header('Content-Type', 'text/plain')
        self.end_headers()
        self.wfile.write(b"")
    
    def _send_not_found(self):
        """Send 404 error - corresponds to C# 404 logic"""
        self.send_response(404)
        self.send_header('Content-Type', 'text/plain')
        self.end_headers()
        self.wfile.write(b"Not Found")


class SimpleHttpServer:
    """HTTP server - corresponds to C# SimpleHttpServer"""
    
    def __init__(self, prefixes: List[str]):
        """Initialize server - corresponds to C# constructor"""
        self.prefixes = prefixes
        self._parameters: SettingParameters = None
        self._updateparams = False
        self.server = None
        self.server_thread = None
        
        # Parse first prefix to get port
        if prefixes:
            # Assume prefix format is "http://127.0.0.1:port/"
            import re
            match = re.search(r':(\d+)/', prefixes[0])
            self.port = int(match.group(1)) if match else 51000
        else:
            self.port = 51000
    
    async def start_async(self):
        """Async start server - corresponds to C# StartAsync"""
        def run_server():
            try:
                # Create HTTP server - corresponds to C# HttpListener
                self.server = HTTPServer(('127.0.0.1', self.port), SimpleHttpHandler)
                
                # Pass server instance reference to handler class
                SimpleHttpHandler.server_instance = self
                
                print("HTTP Server started.")
                self.server.serve_forever()
                
            except Exception as e:
                print(f"Server error: {e}")
                import traceback
                traceback.print_exc()
        
        self.server_thread = threading.Thread(target=run_server, daemon=True)
        self.server_thread.start()
    
    def is_update_param(self) -> bool:
        """Check if parameters are updated - corresponds to C# IsUpdateParam"""
        return self._updateparams
    
    def get_parameters(self) -> SettingParameters:
        """Get parameters and reset update flag - corresponds to C# GetParameters"""
        self._updateparams = False
        return self._parameters
    
    def stop(self):
        """Stop server - corresponds to C# Stop"""
        if self.server:
            self.server.shutdown()
            self.server.server_close()
        print("HTTP Server stopped.")

# Test standalone start function
def start_test_server(port: int = 51000):
    """Start test server"""
    prefixes = [f"http://127.0.0.1:{port}/"]
    server = SimpleHttpServer(prefixes)
    return server

if __name__ == "__main__":
    # Standalone test run
    import time
    import asyncio
    
    async def main():
        server = start_test_server(51000)
        await server.start_async()
        try:
            print("Server running... Press Ctrl+C to stop")
            while True:
                await asyncio.sleep(1)
        except KeyboardInterrupt:
            print("\nShutting down server...")
            server.stop()
    
    asyncio.run(main())
