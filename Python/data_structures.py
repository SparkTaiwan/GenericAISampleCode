"""
Data structure definitions - correspond to structs in C# and C++
"""
from dataclasses import dataclass, field
from typing import List, Optional
import struct

@dataclass
class ROI:
    """Coordinate point structure"""
    x: int = -1
    y: int = -1

@dataclass
class ROIGroup:
    """ROI group with sensitivity, threshold and rectangle coordinates"""
    sensitivity: int = 50
    threshold: int = 50
    rects: List[ROI] = field(default_factory=list)  # Array of 4 corner points

@dataclass 
class SettingParameters:
    """Setting parameters structure"""
    version: str = ""
    analytics_event_api_url: str = ""
    image_width: int = 0
    image_height: int = 0
    jpg_compress: int = 50
    rois: List[ROIGroup] = field(default_factory=list)  # Array of ROI groups

@dataclass
class MMF_Data:
    """Shared memory data structure"""
    header: int = 0x1234
    image_status: int = 0  # 0=no use, 1=new frame, 2=detection got frame
    image_width: int = 0
    image_height: int = 0
    image_size: int = 0
    timestamp: int = 0
    image_data: bytes = field(default_factory=lambda: b'\x00' * (1920 * 1080 * 3))
    footer: int = 0x4321

    def to_bytes(self) -> bytes:
        """Convert structure to byte string for shared memory"""
        # C++ structure: __int64 header + int image_status + int image_width + int image_height + 
        #               int image_size + uint64_t timestamp + unsigned char image_data[1920*1080*3] + __int64 footer
        image_data_padded = self.image_data + b'\x00' * (1920 * 1080 * 3 - len(self.image_data))
        return struct.pack('=q4iQ', self.header, self.image_status, 
                          self.image_width, self.image_height, 
                          self.image_size, self.timestamp) + \
               image_data_padded + struct.pack('=q', self.footer)

    @classmethod
    def from_bytes(cls, data: bytes) -> 'MMF_Data':
        """Create structure from byte string - corresponds to C++ MMF_Data structure"""
        if len(data) < 40:  # Minimum structure size
            raise ValueError("Data too short for MMF_Data structure")
        
        # C++ structure layout: __int64(8) + int(4)*4 + uint64_t(8) = 32 bytes header
        header_size = 8 + 4*4 + 8
        header_data = data[:header_size]
        header, image_status, image_width, image_height, image_size, timestamp = \
            struct.unpack('=q4iQ', header_data)
        
        # Image data starts after header, fixed size 1920*1080*3
        image_data_start = header_size
        image_data_full = data[image_data_start:image_data_start + 1920 * 1080 * 3]
        # Take only actual size data
        image_data = image_data_full[:image_size] if image_size > 0 else b''
        
        # Footer is in last 8 bytes
        footer_data = data[-8:]
        footer = struct.unpack('=q', footer_data)[0] if len(footer_data) >= 8 else 0x4321
        
        return cls(
            header=header,
            image_status=image_status,
            image_width=image_width,
            image_height=image_height,
            image_size=image_size,
            timestamp=timestamp,
            image_data=image_data,
            footer=footer
        )

@dataclass
class AnalyticsResult:
    """Analytics result structure"""
    version: str = "1.2"
    port_num: int = 0
    keyframe: str = ""  # Base64 encoded JPEG image
    timestamp: int = 0
    rois_rects: List[List[dict]] = field(default_factory=list)  # Format: [[{"x":x1,"y":y1}, {"x":x2,"y":y2}], ...]
