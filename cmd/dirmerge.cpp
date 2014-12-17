/*
    Copyright 2008 Brain Research Institute, Melbourne, Australia

    Written by J-Donald Tournier, 27/06/08.

    This file is part of MRtrix.

    MRtrix is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    MRtrix is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with MRtrix.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "command.h"
#include "progressbar.h"
#include "math/vector.h"
#include "math/matrix.h"
#include "math/rng.h"
#include "point.h"
#include "dwi/directions/file.h"
#include "file/ofstream.h"

#include <array>
#include <random>
#include <algorithm>

using namespace MR;
using namespace App;

void usage () {

DESCRIPTION
  + "splice or merge sets of directions over multiple shells into a single set, "
    "in such a way as to maintain near-optimality upon truncation.";

ARGUMENTS
  + Argument ("subsets", "the number of subsets (phase-encode directions) per b-value").type_integer(1,4,10000)
  + Argument ("bvalue files", "the b-value and sets of corresponding files, in order").type_text().allow_multiple()
  + Argument ("out", "the output directions file, with each row listing "
      "the X Y Z gradient directions, the b-value, and an index representing "
      "the phase encode direction").type_file_out();
}


typedef double value_type;
typedef std::array<value_type,3> Direction;
typedef std::vector<Direction> DirectionSet;

struct OutDir {
  Direction d;
  size_t b;
  size_t pe;
};

inline std::ostream& operator<< (std::ostream& stream, const OutDir& d) {
  stream << "[ " << d.d << "], " << d.b << ", " << d.pe << " ]";
  return stream;
}


void run () 
{
  size_t num_subsets = argument[0];


  std::vector<std::vector<DirectionSet>> dirs;
  std::vector<value_type> bvalue ((argument.size() - 2) / (1+num_subsets));
  INFO ("expecting " + str(bvalue.size()) + " b-values");
  if (bvalue.size()*(1+num_subsets) + 2 != argument.size())
    throw Exception ("inconsistent number of arguments");


  // read them in:
  size_t current = 1, nb = 0;
  while (current < argument.size()-1) {
    bvalue[nb] = to<value_type> (argument[current++]);
    std::vector<DirectionSet> d;
    for (size_t i = 0; i < num_subsets; ++i) {
      auto m = DWI::Directions::load_cartesian<value_type> (argument[current++]);
      DirectionSet set;
      for (size_t r = 0; r < m.rows(); ++r)
        set.push_back ({ m(r,0), m(r,1), m(r,2) });
      d.push_back (set);
    }
    INFO ("found b = " + str(bvalue[nb]) + ", " + 
        str ([&]{ std::vector<size_t> s; for (auto& n : d) s.push_back (n.size()); return s; }()) + " volumes");

    dirs.push_back (d);
    ++nb;
  }

  size_t total = [&]{ size_t n = 0; for (auto& d : dirs) for (auto& m : d) n += m.size(); return n; }();
  INFO ("found total of " + str(total) + " volumes") ;


  // pick random direction from first direction set:
  std::random_device rd;
  std::mt19937 rng (rd());
  size_t first = std::uniform_int_distribution<size_t> (0, dirs[0][0].size()-1)(rng);

  
  std::vector<OutDir> merged;

  auto push = [&](size_t b, size_t p, size_t n) 
  { 
    merged.push_back ({ { dirs[b][p][n][0], dirs[b][p][n][1], dirs[b][p][n][2] }, b, p }); 
    dirs[b][p].erase (dirs[b][p].begin()+n); 
  };

  auto energy_pair = [](const Direction& a, const Direction& b) 
  {
    // use combination of mono- and bi-polar electrostatic repulsion models 
    // to ensure adequate coverage of eddy-current space as well as 
    // orientation space. Use a moderate bias, favouring the bipolar model.
    return 1.2 / (
        Math::pow2 (b[0] - a[0]) + 
        Math::pow2 (b[1] - a[1]) + 
        Math::pow2 (b[2] - a[2]) 
        ) + 1.0 / (
        Math::pow2 (b[0] + a[0]) + 
        Math::pow2 (b[1] + a[1]) + 
        Math::pow2 (b[2] + a[2]) 
        );
  };

  auto energy = [&](size_t b, size_t p, size_t n) 
  { 
    value_type E = 0.0;
    for (auto& d : merged) 
      if (d.b == b) 
        E += energy_pair (d.d, dirs[b][p][n]);
    return E;
  };

  auto find_lowest_energy_direction = [&](size_t b, size_t p)
  {
    size_t best = 0;
    value_type bestE = std::numeric_limits<value_type>::max();
    for (size_t n = 0; n < dirs[b][p].size(); ++n) {
      value_type E = energy (b, p, n);
      if (E < bestE) {
        bestE = E;
        best = n;
      }
    }
    return best;
  };



  std::vector<float> fraction;
  for (auto& d : dirs) {
    size_t n = 0;
    for (auto& m : d)
      n += m.size();
    fraction.push_back (float (n) / float (total));
  };

  push (0, 0, first);

  std::vector<size_t> counts (bvalue.size(), 0);
  ++counts[0];

  auto num_for_b = [&](size_t b) {
    size_t n = 0;
    for (auto& d : merged)
      if (d.b == b)
        ++n;
    return n;
  };



  size_t nPE = num_subsets > 1 ? 1 : 0;
  while (merged.size() < total) { 
    // find shell with shortfall in numbers:
    size_t b = 0, n;
    value_type fraction_diff = std::numeric_limits<value_type>::max();
    for (n = 0; n < bvalue.size(); ++n) {
      value_type f_diff = float(num_for_b(n)) / float (merged.size()) - fraction[n];
      if (f_diff < fraction_diff && dirs[n][nPE].size()) {
        fraction_diff = f_diff;
        b = n;
      }
    }

    // find most distant direction for that shell & in the current PE direction:
    n = find_lowest_energy_direction (b, nPE);
    if (dirs[b][nPE].size()) 
      push (b, nPE, n);

    // update PE direction
    ++nPE;
    if (nPE >= num_subsets)
      nPE = 0;
  }





  // write-out:
  
  File::OFStream out (argument[argument.size()-1]);
  for (auto& d : merged) 
    out << MR::printf ("%#10f %#10f %#10f %5d %3d\n", 
        float (d.d[0]), float (d.d[1]), float (d.d[2]), 
        int (bvalue[d.b]), int (d.pe+1));

}


