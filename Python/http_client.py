"""
HTTP client module - corresponds to SimpleHttpClient in C# (lightweight version)
"""
import urllib.request
import urllib.parse
import urllib.error
import asyncio
import json
import threading
from typing import Dict, Any
from data_structures import AnalyticsResult, ROI

class SimpleHttpClient:
    """Lightweight HTTP client"""
    
    def __init__(self):
        self.timeout = 30  # 30 second timeout
    
    async def __aenter__(self):
        return self
    
    async def __aexit__(self, exc_type, exc_val, exc_tb):
        pass
    
    def post_analytics_result_sync(self, url: str, analytics_result: AnalyticsResult) -> str:
        """
        Synchronously send analytics result to specified URL
        """
        try:
            # Convert AnalyticsResult to dictionary format
            data = self._analytics_result_to_dict(analytics_result)
            
            # Convert to JSON string
            json_data = json.dumps(data).encode('utf-8')
            
            # Create request
            req = urllib.request.Request(
                url,
                data=json_data,
                headers={'Content-Type': 'application/json'}
            )
            
            # Send request
            with urllib.request.urlopen(req, timeout=self.timeout) as response:
                return response.read().decode('utf-8')
                
        except urllib.error.URLError as e:
            print(f"URL error: {e}")
            raise
        except Exception as e:
            print(f"HTTP request error: {e}")
            raise
    
    async def post_analytics_result_async(self, url: str, analytics_result: AnalyticsResult) -> str:
        """
        Asynchronously send analytics result to specified URL (using thread pool)
        """
        loop = asyncio.get_event_loop()
        return await loop.run_in_executor(
            None, 
            self.post_analytics_result_sync, 
            url, 
            analytics_result
        )
    
    def _analytics_result_to_dict(self, result: AnalyticsResult) -> Dict[str, Any]:
        """Convert AnalyticsResult to dictionary"""
        return {
            "version": result.version,
            "port_num": result.port_num,
            "keyframe": result.keyframe,
            "timestamp": result.timestamp,
            "rois_rects": result.rois_rects  # Already in [[x1,y1,x2,y2], ...] format
        }
    
    async def close(self):
        """Close client (lightweight version requires no special cleanup)"""
        pass

class HttpRequestQueue:
    """HTTP request queue manager"""
    
    def __init__(self):
        self.queue = asyncio.Queue()
        self.client = SimpleHttpClient()
        self.running = False
        self.worker_task = None
    
    async def start(self):
        """Start queue processor"""
        self.running = True
        await self.client.__aenter__()
        self.worker_task = asyncio.create_task(self._process_queue())
    
    async def stop(self):
        """Stop queue processor"""
        self.running = False
        if self.worker_task:
            self.worker_task.cancel()
            try:
                await self.worker_task
            except asyncio.CancelledError:
                pass
        await self.client.close()
    
    async def enqueue(self, url: str, analytics_result: AnalyticsResult):
        """Add analytics result to queue"""
        await self.queue.put((url, analytics_result))
    
    async def _process_queue(self):
        """Process requests in queue"""
        while self.running:
            try:
                # Wait for items in queue, but set timeout to avoid infinite waiting
                url, result = await asyncio.wait_for(
                    self.queue.get(), timeout=0.5
                )
                
                # Send HTTP request
                try:
                    response = await self.client.post_analytics_result_async(url, result)
                    if response == "":  # Usually no response content, only status code 200
                        print("Detected!! send analytics result to server!!")
                except Exception as e:
                    print(f"Response error: {e}")
                
                self.queue.task_done()
                
            except asyncio.TimeoutError:
                # Timeout is normal, continue loop
                continue
            except Exception as e:
                print(f"Queue processing error: {e}")
                await asyncio.sleep(0.1)
