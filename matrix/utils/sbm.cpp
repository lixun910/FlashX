/*
 * Copyright 2016 Open Connectome Project (http://openconnecto.me)
 * Written by Da Zheng (zhengda1936@gmail.com)
 *
 * This file is part of FlashMatrix.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <chrono>

#include "vertex.h"

#include "vec_store.h"
#include "data_frame.h"
#include "rand_gen.h"
#include "local_vec_store.h"
#include "fm_utils.h"
#include "sparse_matrix.h"

typedef std::pair<off_t, size_t> off_size_t;

using namespace fm;

template<class T>
class matrix
{
	size_t num_rows;
	size_t num_cols;
	std::vector<T> eles;
public:
	matrix(size_t num_rows, size_t num_cols) {
		eles.resize(num_rows * num_cols);
		this->num_rows = num_rows;
		this->num_cols = num_cols;
	}

	void set(T val) {
		for (size_t i = 0; i < eles.size(); i++)
			eles[i] = val;
	}

	size_t get_num_rows() const {
		return num_rows;
	}

	size_t get_num_cols() const {
		return num_cols;
	}

	T sum() const {
		T ret = 0;
		for (size_t i = 0; i < eles.size(); i++)
			ret += eles[i];
		return ret;
	}

	T &operator()(size_t row_idx, size_t col_idx) {
		assert(row_idx < num_rows);
		assert(col_idx < num_cols);
		return eles[row_idx * num_rows + col_idx];
	}

	const T &operator()(size_t row_idx, size_t col_idx) const {
		assert(row_idx < num_rows);
		assert(col_idx < num_cols);
		return eles[row_idx * num_rows + col_idx];
	}
};

matrix<size_t> get_nnzs(const matrix<double> &ps,
		const std::vector<size_t> &block_sizes)
{
	unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
	std::default_random_engine generator (seed);

	printf("cal nnz of each blocks\n");
	matrix<size_t> nnzs(ps.get_num_rows(), ps.get_num_cols());
	for (size_t i = 0; i < nnzs.get_num_rows(); i++)
		for (size_t j = 0; j < nnzs.get_num_cols(); j++) {
			nnzs(i, j) = block_sizes[i] * block_sizes[j] * ps(i, j);
		}
	return nnzs;
}

matrix<off_size_t> cal_offs(const matrix<size_t> &nnzs)
{
	off_t off = 0;
	matrix<off_size_t> locs(nnzs.get_num_rows(), nnzs.get_num_cols());
	for (size_t i = 0; i < locs.get_num_rows(); i++)
		for (size_t j = 0; j < locs.get_num_cols(); j++) {
			locs(i, j).first = off;
			locs(i, j).second = nnzs(i, j);
			off += nnzs(i, j);
		}
	return locs;
}

void rand_shuffle(std::vector<int64_t> data)
{
	unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
	urand_gen_impl<int64_t> gen(0, data.size() - 1, seed);
	std::vector<size_t> src_locs(data.size());
	std::vector<size_t> dst_locs(data.size());
	gen.gen(src_locs.data(), src_locs.size());
	gen.gen(dst_locs.data(), dst_locs.size());
	for (size_t i = 0; i < data.size(); i++) {
		int64_t tmp = data[dst_locs[i]];
		data[dst_locs[i]] = data[src_locs[i]];
		data[src_locs[i]] = tmp;
	}
}

sub_data_frame create_edges_block(size_t nnz, size_t start_row, size_t start_col,
		size_t num_rows, size_t num_cols)
{
	unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
	urand_gen_impl<int64_t> gen(0, num_rows * num_cols - 1, seed);

	// We'll generate more random number than we need.
	std::vector<int64_t> data(nnz * 2);
	gen.gen(data.data(), data.size());
	std::sort(data.begin(), data.end());
	// We lose some edges. Because we generate more random numbers
	// then we need, we should have enough unique random numbers.
	auto last = std::unique(data.begin(), data.end());
	data.resize(last - data.begin());
	assert(data.size() >= nnz);
	if (data.size() > nnz) {
		// We'll random shuffle the unique random numbers and take
		// the first nnz random numbers.
		rand_shuffle(data);
		data.resize(nnz);
	}

	// Here we convert the random numbers to the non-zero entries in the block.
	sub_data_frame df(2);
	df[0] = local_vec_store::ptr(new local_buf_vec_store(0, nnz,
				get_scalar_type<fg::vertex_id_t>(), -1));
	df[1] = local_vec_store::ptr(new local_buf_vec_store(0, nnz,
				get_scalar_type<fg::vertex_id_t>(), -1));
	fg::vertex_id_t *src_arr = (fg::vertex_id_t *) df[0]->get_raw_arr();
	fg::vertex_id_t *dst_arr = (fg::vertex_id_t *) df[1]->get_raw_arr();
	for (size_t i = 0; i < data.size(); i++) {
		// id indicates the location of a non-zero entry when we concatenate
		// all rows.
		size_t id = data[i];
		size_t lrow_id = id / num_cols;
		assert(lrow_id < num_rows);
		size_t lcol_id = id % num_cols;
		src_arr[i] = lrow_id + start_row;
		dst_arr[i] = lcol_id + start_col;
	}
	return df;
}

edge_list::ptr create_sbm(const matrix<double> &ps,
		const std::vector<size_t> &block_sizes, bool in_mem)
{
	matrix<size_t> nnzs = get_nnzs(ps, block_sizes);
	size_t tot_nnz = nnzs.sum();
	detail::vec_store::ptr src = detail::vec_store::create(tot_nnz,
			get_scalar_type<fg::vertex_id_t>(), -1, in_mem);
	detail::vec_store::ptr dst = detail::vec_store::create(tot_nnz,
			get_scalar_type<fg::vertex_id_t>(), -1, in_mem);

	std::vector<size_t> block_offs(block_sizes.size() + 1);
	for (size_t i = 0; i < block_sizes.size(); i++)
		block_offs[i + 1] = block_offs[i] + block_sizes[i];

	matrix<off_size_t> offs = cal_offs(nnzs);
#pragma omp parallel for
	for (size_t i = 0; i < nnzs.get_num_rows() * nnzs.get_num_cols(); i++) {
		size_t block_row = i / nnzs.get_num_rows();
		size_t block_col = i % nnzs.get_num_cols();
		sub_data_frame sub_df = create_edges_block(nnzs(block_row, block_col),
				block_offs[block_row], block_offs[block_col],
				block_sizes[block_row], block_sizes[block_col]);
		off_t off = offs(block_row, block_col).first;
		assert(sub_df[0]->get_length() == offs(block_row, block_col).second);
		src->set_portion(sub_df[0], off);
		dst->set_portion(sub_df[1], off);
	}
	printf("There are %ld edges\n", src->get_length());

	data_frame::ptr df = data_frame::create();
	df->add_vec("src", src);
	df->add_vec("dst", dst);
	return edge_list::create(df, true);
}

int main(int argc, char *argv[])
{
	if (argc < 6) {
		fprintf(stderr, "sbm conf_file #vertices #edges #clusters matrix_name");
		return -1;
	}
	std::string conf_file = argv[1];
	size_t num_vertices = atol(argv[2]);
	size_t num_edges = atol(argv[3]);
	size_t num_clusters = atoi(argv[4]);
	std::string matrix_name = argv[5];

	config_map::ptr configs = config_map::create(conf_file);
	init_flash_matrix(configs);

	double r = 5;
	size_t cluster_size = num_vertices / num_clusters;
	// The number of edges inside clusters is the same as the ones outside clusters.
	double p0 = ((double) num_edges)
		/ ((1 + r) * num_vertices * (num_vertices - cluster_size));
	double p1 = ((double) num_edges * r) / ((1 + r) * cluster_size * num_vertices);
#if 0
	double p0 = ((double) num_edges) / ((num_vertices - cluster_size) * num_vertices
			+ 100 * cluster_size * num_vertices);
	double p1 = 100 * p0;
#endif
	printf("p0 = %g, p1 = %g\n", p0, p1);
	printf("aprox %ld edges in clusters, %ld edges outside clusters\n",
			(long) (p1 * cluster_size * num_vertices),
			(long) (p0 * (num_vertices - cluster_size) * num_vertices));

	std::vector<size_t> cluster_sizes(num_clusters);
	assert(num_vertices % num_clusters == 0);
	for (size_t i = 0; i < cluster_sizes.size(); i++)
		cluster_sizes[i] = num_vertices / num_clusters;

	matrix<double> ps(num_clusters, num_clusters);
	ps.set(p0);
	for (size_t i = 0; i < ps.get_num_rows(); i++)
		ps(i, i) = p1;

	{
		struct timeval start, end;
		gettimeofday(&start, NULL);
		edge_list::ptr el = create_sbm(ps, cluster_sizes, true);
		gettimeofday(&end, NULL);
		printf("It takes %f seconds to create edges\n", time_diff(start, end));

		auto ret = create_2d_matrix(el, block_2d_size(16 * 1024, 16 * 1024),
				NULL);
		ret.first->dump(matrix_name + ".mat_idx");
		ret.second->dump(matrix_name + ".mat");
	}

	destroy_flash_matrix();
}
