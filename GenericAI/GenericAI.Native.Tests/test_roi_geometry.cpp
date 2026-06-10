#include "CppUnitTest.h"

#include "roi_geometry.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GenericAINativeTests
{
    TEST_CLASS(RoiGeometryTest)
    {
    public:
        TEST_METHOD(BoxFullyInsideRoi_Overlaps)
        {
            DetectionRect b{ 10, 10, 20, 20 };
            ROIRect r{ 0, 0, 100, 100 };
            Assert::IsTrue(BoxOverlapsRoi(b, r));
        }

        TEST_METHOD(BoxFullyOutsideRoi_NoOverlap)
        {
            ROIRect r{ 50, 50, 100, 100 };
            // Left / right / above / below.
            Assert::IsFalse(BoxOverlapsRoi(DetectionRect{ 0, 60, 10, 10 }, r));
            Assert::IsFalse(BoxOverlapsRoi(DetectionRect{ 200, 60, 10, 10 }, r));
            Assert::IsFalse(BoxOverlapsRoi(DetectionRect{ 60, 0, 10, 10 }, r));
            Assert::IsFalse(BoxOverlapsRoi(DetectionRect{ 60, 200, 10, 10 }, r));
        }

        TEST_METHOD(BoxTouchingRoiEdge_NoOverlap)
        {
            ROIRect r{ 50, 50, 100, 100 };
            // Edge-to-edge contact is exclusive (<= / >= in the implementation).
            Assert::IsFalse(BoxOverlapsRoi(DetectionRect{ 40, 60, 10, 10 }, r));   // b right edge == r left edge
            Assert::IsFalse(BoxOverlapsRoi(DetectionRect{ 100, 60, 10, 10 }, r));  // b left edge == r right edge
            Assert::IsFalse(BoxOverlapsRoi(DetectionRect{ 60, 40, 10, 10 }, r));   // b bottom edge == r top edge
            Assert::IsFalse(BoxOverlapsRoi(DetectionRect{ 60, 100, 10, 10 }, r));  // b top edge == r bottom edge
        }

        TEST_METHOD(InvertedRoiCoordinates_SameResultAsNormalized)
        {
            DetectionRect inside{ 60, 60, 10, 10 };
            DetectionRect outside{ 0, 0, 10, 10 };
            ROIRect normal{ 50, 50, 100, 100 };
            ROIRect inverted{ 100, 100, 50, 50 };
            Assert::AreEqual(BoxOverlapsRoi(inside, normal), BoxOverlapsRoi(inside, inverted));
            Assert::AreEqual(BoxOverlapsRoi(outside, normal), BoxOverlapsRoi(outside, inverted));
            Assert::IsTrue(BoxOverlapsRoi(inside, inverted));
            Assert::IsFalse(BoxOverlapsRoi(outside, inverted));
        }

        TEST_METHOD(PartialOverlap_Overlaps)
        {
            DetectionRect b{ 40, 40, 20, 20 };   // spans 40..60, crosses ROI corner at 50,50
            ROIRect r{ 50, 50, 100, 100 };
            Assert::IsTrue(BoxOverlapsRoi(b, r));
        }
    };
}
