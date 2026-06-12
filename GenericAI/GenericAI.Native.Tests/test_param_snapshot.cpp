#include "CppUnitTest.h"

#include "param_snapshot.h"

#include <cstring>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GenericAINativeTests
{
    namespace
    {
        // All ROI slots empty: x >= 0 is the "point present" test in Apply,
        // so -1 marks every slot unused.
        GAI_Settings MakeEmptySettings()
        {
            GAI_Settings s;
            std::memset(&s, 0, sizeof(s));
            for (int i = 0; i < 10; ++i)
            {
                for (int j = 0; j < 10; ++j)
                {
                    s.rois[i][j].x = -1;
                    s.rois[i][j].y = -1;
                }
            }
            return s;
        }

        // Axis-aligned 4-point rectangle (x1,y1)-(x2,y2); Apply derives the
        // bounding rect from pts[0] and pts[2].
        void FillRectGroup(GAI_Settings& s, int group, int x1, int y1, int x2, int y2)
        {
            s.rois[group][0] = GAI_Roi{ x1, y1 };
            s.rois[group][1] = GAI_Roi{ x2, y1 };
            s.rois[group][2] = GAI_Roi{ x2, y2 };
            s.rois[group][3] = GAI_Roi{ x1, y2 };
        }
    }

    TEST_CLASS(ParamSnapshotTest)
    {
    public:
        TEST_METHOD(DefaultParams_MatchLegacyMotionDefaults)
        {
            gai::ParamSnapshot snap;
            gai::ParamSnapshot::View v = snap.Take();
            Assert::AreEqual(25, v.params.threshold);
            Assert::AreEqual(50, v.params.sensitivity);
            Assert::IsTrue(v.roi_rects.empty());
        }

        TEST_METHOD(GroupWithFewerThanThreePoints_IsSkipped)
        {
            GAI_Settings s = MakeEmptySettings();
            s.rois[0][0] = GAI_Roi{ 0, 0 };
            s.rois[0][1] = GAI_Roi{ 100, 100 };

            gai::ParamSnapshot snap;
            snap.Apply(s);

            gai::ParamSnapshot::View v = snap.Take();
            Assert::IsTrue(v.roi_rects.empty());
            Assert::IsTrue(v.original_roi_points.empty());
        }

        TEST_METHOD(ParamsComeFromFirstValidGroup_NotIndexZero)
        {
            GAI_Settings s = MakeEmptySettings();
            // group0 invalid (2 points), group1 valid.
            s.rois[0][0] = GAI_Roi{ 0, 0 };
            s.rois[0][1] = GAI_Roi{ 10, 10 };
            FillRectGroup(s, 1, 0, 0, 100, 50);
            s.threshold[0] = 11;
            s.sensitivity[0] = 22;
            s.threshold[1] = 77;
            s.sensitivity[1] = 88;

            gai::ParamSnapshot snap;
            snap.Apply(s);

            gai::ParamSnapshot::View v = snap.Take();
            Assert::AreEqual(1, static_cast<int>(v.roi_rects.size()));
            Assert::AreEqual(77, v.params.threshold);
            Assert::AreEqual(88, v.params.sensitivity);
        }

        TEST_METHOD(MultipleValidGroups_FirstValidWinsParams)
        {
            GAI_Settings s = MakeEmptySettings();
            FillRectGroup(s, 2, 0, 0, 50, 50);
            FillRectGroup(s, 5, 60, 60, 90, 90);
            s.threshold[2] = 33;
            s.sensitivity[2] = 44;
            s.threshold[5] = 99;
            s.sensitivity[5] = 99;

            gai::ParamSnapshot snap;
            snap.Apply(s);

            gai::ParamSnapshot::View v = snap.Take();
            Assert::AreEqual(2, static_cast<int>(v.roi_rects.size()));
            Assert::AreEqual(33, v.params.threshold);
            Assert::AreEqual(44, v.params.sensitivity);
        }

        TEST_METHOD(AllInvalidApply_KeepsPreviousParams)
        {
            GAI_Settings valid = MakeEmptySettings();
            FillRectGroup(valid, 0, 0, 0, 100, 100);
            valid.threshold[0] = 77;
            valid.sensitivity[0] = 66;

            gai::ParamSnapshot snap;
            snap.Apply(valid);
            snap.Apply(MakeEmptySettings());

            gai::ParamSnapshot::View v = snap.Take();
            Assert::IsTrue(v.roi_rects.empty());        // rects always follow the new settings
            Assert::AreEqual(77, v.params.threshold);   // params keep the last valid tuning
            Assert::AreEqual(66, v.params.sensitivity);
        }

        TEST_METHOD(ImageDims_CarriedIntoParams)
        {
            GAI_Settings s = MakeEmptySettings();
            FillRectGroup(s, 0, 0, 0, 100, 100);
            s.image_width = 1920;
            s.image_height = 1080;

            gai::ParamSnapshot snap;
            // Fresh snapshot has no reference space yet.
            Assert::AreEqual(0, snap.Take().params.image_width);
            Assert::AreEqual(0, snap.Take().params.image_height);

            snap.Apply(s);

            gai::ParamSnapshot::View v = snap.Take();
            Assert::AreEqual(1920, v.params.image_width);
            Assert::AreEqual(1080, v.params.image_height);
        }

        TEST_METHOD(ImageDims_UpdateEvenWhenAllGroupsInvalid)
        {
            GAI_Settings valid = MakeEmptySettings();
            FillRectGroup(valid, 0, 0, 0, 100, 100);
            valid.threshold[0] = 77;
            valid.sensitivity[0] = 66;
            valid.image_width = 1920;
            valid.image_height = 1080;

            GAI_Settings invalid = MakeEmptySettings();
            invalid.image_width = 640;
            invalid.image_height = 360;

            gai::ParamSnapshot snap;
            snap.Apply(valid);
            snap.Apply(invalid);

            gai::ParamSnapshot::View v = snap.Take();
            Assert::AreEqual(77, v.params.threshold);     // tuning keeps the last valid set
            Assert::AreEqual(66, v.params.sensitivity);
            Assert::AreEqual(640, v.params.image_width);  // reference space follows the payload
            Assert::AreEqual(360, v.params.image_height);
        }

        TEST_METHOD(RectDerivedFromFirstAndThirdPoint)
        {
            GAI_Settings s = MakeEmptySettings();
            FillRectGroup(s, 0, 12, 34, 56, 78);

            gai::ParamSnapshot snap;
            snap.Apply(s);

            gai::ParamSnapshot::View v = snap.Take();
            Assert::AreEqual(1, static_cast<int>(v.roi_rects.size()));
            Assert::AreEqual(12, v.roi_rects[0].x1);
            Assert::AreEqual(34, v.roi_rects[0].y1);
            Assert::AreEqual(56, v.roi_rects[0].x2);
            Assert::AreEqual(78, v.roi_rects[0].y2);
            Assert::AreEqual(1, static_cast<int>(v.original_roi_points.size()));
            Assert::AreEqual(4, static_cast<int>(v.original_roi_points[0].size()));
        }
    };
}
