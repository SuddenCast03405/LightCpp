// FindScanTests — the resume position all three find/replace scans share.
//
// The bug this pins down (R2-30) was not in the policy but in it being
// written three times and fixed once: Replace All stepped past a zero-width
// match by hand, Find Next and the match counter did not. A pattern like
// "\b"/"x*"/"^" therefore left the caret parked on the same spot and pinned
// the counter at "9999+".

#include "LightCpp/App/FindScan.hpp"

#include <gtest/gtest.h>

using LightCpp::App::FindScan::resumeAfter;

TEST(FindScan, AMatchResumesAtItsEnd)
{
    // [3, 7) in a 10-character document -> the scan continues at 7.
    EXPECT_EQ(resumeAfter(3, 4, 10), 7);
}

TEST(FindScan, AZeroWidthMatchStepsOverOneCharacter)
{
    EXPECT_EQ(resumeAfter(0, 0, 10), 1);
    EXPECT_EQ(resumeAfter(4, 0, 10), 5);
}

TEST(FindScan, BackwardStepsTheOtherWay)
{
    EXPECT_EQ(resumeAfter(4, 0, 10, /*backward=*/true), 3);
    EXPECT_EQ(resumeAfter(4, 2, 10, /*backward=*/true), 6);
}

TEST(FindScan, NoRoomLeftIsReportedAsMinusOne)
{
    // A zero-width match on the last position: forward there is nowhere to
    // go, which is what tells the search to wrap and the loops to stop.
    EXPECT_EQ(resumeAfter(10, 0, 10), -1);
    EXPECT_EQ(resumeAfter(0, 0, 10, /*backward=*/true), -1);
}

TEST(FindScan, TheLastRealMatchStillResumesAtTheDocumentEnd)
{
    // The step is only for zero-width matches: a one-character match ending
    // on the last position resumes at the end of the document, which is a
    // legal cursor position, not "wrap".
    EXPECT_EQ(resumeAfter(9, 1, 10), 10);
}

TEST(FindScan, AnEmptyDocumentHasNoResumePosition)
{
    // docEnd is 0 for a document holding only its final block separator.
    EXPECT_EQ(resumeAfter(0, 0, 0), -1);
}
