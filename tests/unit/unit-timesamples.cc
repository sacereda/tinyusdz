#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-timesamples.h"
#include "prim-types.hh"
#include "math-util.inc"

using namespace tinyusdz;

void timesamples_test(void) {

  {
    value::token tok1("bora");
    value::token tok2("muda");

    Animatable<value::token> toks;
    toks.add_sample(0, tok1);
    toks.add_sample(10, tok2);

    {
      value::token tok;
      TEST_CHECK(toks.get(value::TimeCode::Default(), &tok));
      // return the value of the first item(= timecode 0)
      TEST_CHECK(tok.str() == "bora");
    }      

    // Held interpolation
    {
      value::token tok;
      TEST_CHECK(toks.get(0.0, &tok));
      TEST_CHECK(tok.str() == "bora");

      TEST_CHECK(toks.get(-1.0, &tok));
      TEST_CHECK(tok.str() == "bora");

      TEST_CHECK(toks.get(1.0, &tok));
      TEST_CHECK(tok.str() == "bora");

      TEST_CHECK(toks.get(10.0, &tok));
      TEST_CHECK(tok.str() == "muda");

      TEST_CHECK(toks.get(1000.0, &tok));
      TEST_CHECK(tok.str() == "muda");
    }      
  }

  {
    Animatable<float> samples;
    samples.add_sample(0, 0.0f);
    samples.add_sample(1, 10.0f);

    {
      float f;
      TEST_CHECK(samples.get(value::TimeCode::Default(), &f));
      // return the value of the first item(= timecode 0)
      TEST_CHECK(math::is_close(f, 0.0f));
    }      

    // Linear interpolation
    {
      float f;
      TEST_CHECK(samples.get(0.0, &f));
      TEST_CHECK(math::is_close(f, 0.0f));

      TEST_CHECK(samples.get(0.5, &f));
      TEST_CHECK(math::is_close(f, 5.0f));

      TEST_CHECK(samples.get(1.0, &f));
      TEST_CHECK(math::is_close(f, 10.0f));

    }      
  }

  {
    primvar::PrimVar pbar;
    value::TimeSamples ts;
    ts.add_sample(0, value::Value(0.0f));
    ts.add_sample(1, value::Value(10.0f));
    pbar.set_timesamples(ts);
    pbar.set_value(2000.0f); // default value

    {
      float f;
      TEST_CHECK(pbar.get_interpolated_value(value::TimeCode::Default(), value::TimeSampleInterpolationType::Held, &f));
      // return the value of the first item(= timecode 0)
      TEST_CHECK(math::is_close(f, 2000.0f));
    }      

    // Linear interpolation
    {
      float f;
      TEST_CHECK(pbar.get_interpolated_value(-10.0, value::TimeSampleInterpolationType::Linear, &f));
      TEST_CHECK(math::is_close(f, 0.0f));

      TEST_CHECK(pbar.get_interpolated_value(0.0, value::TimeSampleInterpolationType::Linear, &f));
      TEST_CHECK(math::is_close(f, 0.0f));

      TEST_CHECK(pbar.get_interpolated_value(0.5, value::TimeSampleInterpolationType::Linear, &f));
      TEST_CHECK(math::is_close(f, 5.0f));

      TEST_CHECK(pbar.get_interpolated_value(1.0, value::TimeSampleInterpolationType::Linear, &f));
      TEST_CHECK(math::is_close(f, 10.0f));

      TEST_CHECK(pbar.get_interpolated_value(value::TimeCode::Default(), value::TimeSampleInterpolationType::Linear, &f));
      TEST_CHECK(math::is_close(f, 2000.0f));
    }      
  }


}
