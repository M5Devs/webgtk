// Minimal stand-in for the GoogleTest header of the same name.
//
// The pruned engine archive drops //testing, but //base/gtest_prod_util.h
// includes this file to define FRIEND_TEST. The compile-verification build
// (scripts/verify_engine.sh) compiles engine headers without building any test
// target, so the macro alone is enough.
#ifndef GOOGLETEST_INCLUDE_GTEST_GTEST_PROD_H_
#define GOOGLETEST_INCLUDE_GTEST_GTEST_PROD_H_

#define FRIEND_TEST(test_case_name, test_name) \
  friend class test_case_name##_##test_name##_Test

#endif  // GOOGLETEST_INCLUDE_GTEST_GTEST_PROD_H_
