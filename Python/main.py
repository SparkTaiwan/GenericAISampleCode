"""
Main program - corresponds to Program.cs in C#
"""
import asyncio
import sys
import time
import copy
import os
from typing import List, Tuple
from data_structures import AnalyticsResult, ROI, SettingParameters
from analytics_engine import Initialize, SettingParameters, registerCallback, unregisterCallback, Deinitialize
from http_server import SimpleHttpServer
from http_client import HttpRequestQueue
from image_processor import ImageProcessor

class SampleWrapperMain:
    """Main program class"""
    
    def __init__(self):
        # Use hardcoded default values instead of config
        self.port_num = 51000  # Default port
        self.url = ""
        self.jpg_compress = 50  # Default JPG compression quality
        self.http_request_queue = HttpRequestQueue()
        self.http_server: SimpleHttpServer = None
        self.running = True
        self.debug_mode = False
        
        # Store the main event loop during initialization
        self.main_event_loop = asyncio.get_event_loop()
    
    def callback_function(self, channel_id: int, width: int, height: int, 
                         image_frame: bytes, image_size: int, timestamp: int,
                         rois_rects: List[List[ROI]], rois_count: int, node_count: int,
                         detections: List[Tuple[int, int, int, int]] = None):
        """
        Python version of C++ event callback function
        Send image frame when analysis detects something
        """
        try:
            if self.debug_mode:
                print(f"[DEBUG] Detection callback triggered - received shared memory data:")
                print(f"  - Channel ID: {channel_id}")
                print(f"  - Image size: {width}x{height}")
                print(f"  - Image size: {image_size} bytes")
                print(f"  - Timestamp: {timestamp}")
                print(f"  - ROI group count: {rois_count}")
                print(f"  - Node count: {node_count}")
                
                # Print details of first few ROIs
                for i, roi_group in enumerate(rois_rects[:3]):  # Print first 3 groups
                    if isinstance(roi_group, ROI):
                        print(f"  - ROI group {i}: 1 region")
                        print(f"    ROI[0]: x={roi_group.x}, y={roi_group.y}")
                    elif roi_group:
                        print(f"  - ROI group {i}: {len(roi_group)} regions")
                        for j, roi in enumerate(roi_group[:2]):  # Print first 2 per group
                            print(f"    ROI[{j}]: x={roi.x}, y={roi.y}")
            
            # Convert YUV420 to JPEG then to Base64
            base64_jpeg_string = ImageProcessor.yuv420_to_base64_jpeg(
                image_frame, width, height, self.jpg_compress, detections, self.debug_mode
            )
            
            if self.debug_mode:
                print(f"  - Base64 JPEG length: {len(base64_jpeg_string)} characters")
            
            # Convert detections to rois_rects format: [[{"x":x1,"y":y1}, {"x":x2,"y":y2}], ...]
            detection_rects = []
            if detections:
                for x, y, w, h in detections:
                    # Convert (x, y, w, h) to ROI point format
                    x1, y1 = x, y
                    x2, y2 = x + w, y + h
                    roi_points = [
                        {"x": x1, "y": y1},  # Top-left
                        {"x": x2, "y": y1},  # Top-right
                        {"x": x2, "y": y2},  # Bottom-right
                        {"x": x1, "y": y2}   # Bottom-left
                    ]
                    detection_rects.append(roi_points)
                
                if self.debug_mode:
                    print(f"  - Formatted detection rects: {detection_rects}")
            
            # Print the final rois_rects being sent
            print(f"[DEBUG] Sending rois_rects: {detection_rects}")
            
            # Create analytics result
            analytics_result = AnalyticsResult(
                version="1.2",
                port_num=self.port_num,
                keyframe=base64_jpeg_string,
                timestamp=timestamp,
                rois_rects=detection_rects
            )
            
            # Add analytics result to queue for processing
            asyncio.run_coroutine_threadsafe(
                self.http_request_queue.enqueue(self.url, analytics_result), self.main_event_loop
            )
            
            if self.debug_mode:
                print(f"  - Added to send queue, target URL: {self.url}")
                print("Detected!! send analytics result to server!!")
            
        except Exception as e:
            print(f"Callback error: {e}")
            if self.debug_mode:
                import traceback
                print(f"[DEBUG] Detailed error information:")
                traceback.print_exc()
    
    async def start_server_tasks(self):
        """Start server-related tasks"""
        try:
            # Start HTTP request queue processor
            await self.http_request_queue.start()
            print("HTTP request queue started")
            
            # Start HTTP server - corresponds to C# StartAsync
            await self.http_server.start_async()
            
            # Wait for server to start and check status
            await asyncio.sleep(2)
            
            # Check if server started successfully
            if self.http_server.server_thread and self.http_server.server_thread.is_alive():
                print("HTTP server started successfully")
            else:
                print("Failed to start HTTP server")
                raise RuntimeError("HTTP server failed to start")
                
        except Exception as e:
            print(f"Error starting server tasks: {e}")
            raise
    
    async def parameter_monitoring_task(self):
        """Parameter monitoring task - wait for valid parameter settings before initializing analytics engine"""
        print("[LOG] Waiting for valid parameter settings...")
        
        while self.running:
            if self.http_server.is_update_param():
                parameters = self.http_server.get_parameters()
                
                # Check if parameters are valid and complete
                if (parameters and 
                    parameters.analytics_event_api_url and 
                    parameters.image_width > 0 and 
                    parameters.image_height > 0):
                    
                    print(f"[LOG] Received valid parameter settings!")
                    print(f"  - API URL: {parameters.analytics_event_api_url}")
                    print(f"  - Image size: {parameters.image_width}x{parameters.image_height}")
                    
                    # Set JPEG compression quality
                    if parameters.jpg_compress > 0:
                        self.jpg_compress = parameters.jpg_compress
                    
                    self.url = parameters.analytics_event_api_url
                    
                    # Register callback function
                    registerCallback(self.callback_function)
                    print("[LOG] Callback function registered")                   
                    
                    # Set parameters
                    print("[LOG] Setting parameters")
                    SettingParameters(parameters)
                                      
                    print("[LOG] System is fully ready!")
                    return  # Parameter setting complete, end monitoring task
                    
                else:
                    print(f"[WARNING] Received parameters but incomplete, continue waiting...")
                    if parameters:
                        print(f"  - API URL: {parameters.analytics_event_api_url or 'not set'}")
                        print(f"  - Image size: {parameters.image_width}x{parameters.image_height}")
            
            await asyncio.sleep(1)  # Check every second
    
    async def run(self, args: List[str]):
        """Main run method"""
        try:
            print("Usage: SampleWrapper.exe port=<httpPort>")
            print("Python Sample Wrapper v1.0")
            
            print("Using default configuration")
            
            # Parse command line arguments
            shared_memory_port = self.port_num  # Default shared memory port same as HTTP port
            
            if args:
                for arg in args:
                    if arg.startswith("port="):
                        try:
                            self.port_num = int(arg.split("=")[1])
                            print(f"Port number: {self.port_num}")
                        except ValueError:
                            print("Invalid Input. Use default port")
                            self.port_num = 51000  # Use hardcoded default
                    elif arg.startswith("shm_port="):  # New: specify shared memory port
                        try:
                            shared_memory_port = int(arg.split("=")[1])
                            print(f"Shared memory port: {shared_memory_port}")
                        except ValueError:
                            print("Invalid shared memory port. Using HTTP port")
                            shared_memory_port = self.port_num
                    elif arg == "debug" or arg == "--debug":
                        self.debug_mode = True
                        print("Debug mode enabled - save detection images when objects are detected")
                    elif arg.startswith("debug="):
                        debug_value = arg.split("=")[1].lower()
                        self.debug_mode = debug_value in ['true', '1', 'yes', 'on']
                        if self.debug_mode:
                            print("Debug mode enabled - save detection images when objects are detected")
                        else:
                            print("Debug mode disabled")
            
            http_server_url = f"http://127.0.0.1:{self.port_num}/"
            print(f"httpServerUrl: {http_server_url}")
            
            # Initialize analytics engine - use same port (corresponds to C# Initialize(httpServerPort))
            Initialize(self.port_num)
            
            # Set a custom detector if needed
            from analytics_engine import set_detector
            from detectors import get_default_detector
            set_detector(get_default_detector())
            
            # Register callback function
            registerCallback(self.callback_function)
            print("Registered callback")
            
            # Create HTTP server - corresponds to C# constructor
            prefixes = [http_server_url]
            self.http_server = SimpleHttpServer(prefixes)
            
            try:
                # Start server tasks
                await self.start_server_tasks()
                
                # Start parameter monitoring task
                monitoring_task = asyncio.create_task(self.parameter_monitoring_task())
                
                print("All services started. Press Ctrl+C to stop.")
                
                # Keep main program running
                while self.running:
                    await asyncio.sleep(1)
                
                # Cleanup
                monitoring_task.cancel()
                
            except KeyboardInterrupt:
                print("\\nShutting down...")
            except Exception as e:
                print(f"Error: {e}")
                import traceback
                print("Detailed error information:")
                traceback.print_exc()
            finally:
                await self.cleanup()
                
        except Exception as e:
            print(f"Critical error in run method: {e}")
            import traceback
            print("Detailed error information:")
            traceback.print_exc()
            self.running = False
    
    async def cleanup(self):
        """Clean up resources"""
        self.running = False
        
        # Stop HTTP request queue
        await self.http_request_queue.stop()
        
        # Unregister callback
        unregisterCallback()
        Deinitialize()
        
        # Stop HTTP server
        if self.http_server:
            self.http_server.stop()
        
        print("Cleanup completed")

async def main():
    """Main entry point"""
    wrapper = SampleWrapperMain()
    await wrapper.run(sys.argv[1:])

if __name__ == "__main__":
    # Prevent re-execution in PyInstaller
    import sys
    if getattr(sys, 'frozen', False):
        # Running in PyInstaller bundle
        import multiprocessing
        multiprocessing.freeze_support()
    
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\\nProgram interrupted by user")
    except Exception as e:
        print(f"Program error: {e}")
