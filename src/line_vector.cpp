/*
Modified Line algorithm with vector inputs and outpus. 

Contact Author: Jian Tang, Microsoft Research, jiatang@microsoft.com, tangjianpku@gmail.com
Publication: Jian Tang, Meng Qu, Mingzhe Wang, Ming Zhang, Jun Yan, Qiaozhu Mei. "LINE: Large-scale Information Network Embedding". In WWW 2015.
*/

// Format of the training file:
//
// The training file contains serveral lines, each line represents a DIRECTED edge in the network.
// More specifically, each line has the following format "<u> <v> <w>", meaning an edge from <u> to <v> with weight as <w>.
// <u> <v> and <w> are seperated by ' ' or '\t' (blank or tab)
// For UNDIRECTED edge, the user should use two DIRECTED edges to represent it.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <gsl/gsl_rng.h>
#include <vector> 
#include <string> 
#include <R.h>

#define MAX_STRING 100
#define SIGMOID_BOUND 6
#define NEG_SAMPLING_POWER 0.75

static const int hash_table_size = 30000000;
static const int neg_table_size = 1e8;
static const int sigmoid_table_size = 1000;

typedef float real;                    // Precision of float numbers

struct ClassVertex {
	double degree;
	char *name;
};

static char network_file[MAX_STRING], embedding_file[MAX_STRING];
static struct ClassVertex *vertex;
static int is_binary = 0, num_threads = 1, order = 2, dim = 100, num_negative = 5;
static int *vertex_hash_table, *neg_table;
static int max_num_vertices = 1000, num_vertices = 0;
static long long total_samples = 1, current_sample_count = 0, num_edges = 0;
static real init_rho = 0.025, rho;
static real *emb_vertex, *emb_context, *sigmoid_table;

static int *edge_source_id, *edge_target_id;
static double *edge_weight;
static int malloc_exit = 0;

// Parameters for edge sampling
static long long *alias;
static double *prob;

static const gsl_rng_type * gsl_T;
static gsl_rng * gsl_r;


/* Build a hash table, mapping each vertex name to a unique vertex id */
static unsigned int Hash(char *key)
{
	unsigned int seed = 131;
	unsigned int hash = 0;
	while (*key)
	{
		hash = hash * seed + (*key++);
	}
	return hash % hash_table_size;
}

static void InitHashTable()
{
	vertex_hash_table = (int *)malloc(hash_table_size * sizeof(int));
	for (int k = 0; k != hash_table_size; k++) vertex_hash_table[k] = -1;
}

static void InsertHashTable(char *key, int value)
{
	int addr = Hash(key);
	while (vertex_hash_table[addr] != -1) addr = (addr + 1) % hash_table_size;
	vertex_hash_table[addr] = value;
}

static int SearchHashTable(char *key)
{
	int addr = Hash(key);
	while (1)
	{
		if (vertex_hash_table[addr] == -1) return -1;
		if (!strcmp(key, vertex[vertex_hash_table[addr]].name)) return vertex_hash_table[addr];
		addr = (addr + 1) % hash_table_size;
	}
	return -1;
}

/* Add a vertex to the vertex set */
static int AddVertex(char *name)
{
	int length = strlen(name) + 1;
	if (length > MAX_STRING) length = MAX_STRING;
	vertex[num_vertices].name = (char *)calloc(length, sizeof(char));
	strncpy(vertex[num_vertices].name, name, length-1);
	vertex[num_vertices].degree = 0;
	num_vertices++;
	if (num_vertices + 2 >= max_num_vertices)
	{
		max_num_vertices += 1000;
		vertex = (struct ClassVertex *)realloc(vertex, max_num_vertices * sizeof(struct ClassVertex));
	}
	InsertHashTable(name, num_vertices - 1);
	return num_vertices - 1;
}

/* The alias sampling algorithm, which is used to sample an edge in O(1) time. */
static void InitAliasTable()
{
	alias = (long long *)malloc(num_edges*sizeof(long long));
	prob = (double *)malloc(num_edges*sizeof(double));
	if (alias == NULL || prob == NULL)
	{
		Rprintf("Error: memory allocation failed!\n");
        malloc_exit = 1;
        return;
    }

	double *norm_prob = (double*)malloc(num_edges*sizeof(double));
	long long *large_block = (long long*)malloc(num_edges*sizeof(long long));
	long long *small_block = (long long*)malloc(num_edges*sizeof(long long));
	if (norm_prob == NULL || large_block == NULL || small_block == NULL)
	{
		Rprintf("Error: memory allocation failed!\n");
	    malloc_exit = 1;
        return;
    }

	double sum = 0;
	long long cur_small_block, cur_large_block;
	long long num_small_block = 0, num_large_block = 0;

	for (long long k = 0; k != num_edges; k++) sum += edge_weight[k];
	for (long long k = 0; k != num_edges; k++) norm_prob[k] = edge_weight[k] * num_edges / sum;

	for (long long k = num_edges - 1; k >= 0; k--)
	{
		if (norm_prob[k]<1)
			small_block[num_small_block++] = k;
		else
			large_block[num_large_block++] = k;
	}

	while (num_small_block && num_large_block)
	{
		cur_small_block = small_block[--num_small_block];
		cur_large_block = large_block[--num_large_block];
		prob[cur_small_block] = norm_prob[cur_small_block];
		alias[cur_small_block] = cur_large_block;
		norm_prob[cur_large_block] = norm_prob[cur_large_block] + norm_prob[cur_small_block] - 1;
		if (norm_prob[cur_large_block] < 1)
			small_block[num_small_block++] = cur_large_block;
		else
			large_block[num_large_block++] = cur_large_block;
	}

	while (num_large_block) prob[large_block[--num_large_block]] = 1;
	while (num_small_block) prob[small_block[--num_small_block]] = 1;

	free(norm_prob);
	free(small_block);
	free(large_block);
}

static long long SampleAnEdge(double rand_value1, double rand_value2)
{
	long long k = (long long)num_edges * rand_value1;
	return rand_value2 < prob[k] ? k : alias[k];
}

/* Initialize the vertex embedding and the context embedding */
static void InitVector()
{
	long long a, b;

	a = posix_memalign((void **)&emb_vertex, 128, (long long)num_vertices * dim * sizeof(real));
	if (emb_vertex == NULL) { malloc_exit = 1; Rprintf("Error: memory allocation failed\n"); return; }
	for (b = 0; b < dim; b++) for (a = 0; a < num_vertices; a++)
		emb_vertex[a * dim + b] = (unif_rand() - 0.5) / dim;

	a = posix_memalign((void **)&emb_context, 128, (long long)num_vertices * dim * sizeof(real));
	if (emb_context == NULL) { malloc_exit = 1; Rprintf("Error: memory allocation failed\n"); return; }
	for (b = 0; b < dim; b++) for (a = 0; a < num_vertices; a++)
		emb_context[a * dim + b] = 0;
}

/* Sample negative vertex samples according to vertex degrees */
static void InitNegTable()
{
	double sum = 0, cur_sum = 0, por = 0;
	int vid = 0;
	neg_table = (int *)malloc(neg_table_size * sizeof(int));
	for (int k = 0; k != num_vertices; k++) sum += pow(vertex[k].degree, NEG_SAMPLING_POWER);
	for (int k = 0; k != neg_table_size; k++)
	{
		if ((double)(k + 1) / neg_table_size > por)
		{
			cur_sum += pow(vertex[vid].degree, NEG_SAMPLING_POWER);
			por = cur_sum / sum;
			vid++;
		}
		neg_table[k] = vid - 1;
	}
}

/* Fastly compute sigmoid function */
static void InitSigmoidTable()
{
	real x;
	sigmoid_table = (real *)malloc((sigmoid_table_size + 1) * sizeof(real));
	for (int k = 0; k != sigmoid_table_size; k++)
	{
		x = 2 * SIGMOID_BOUND * k / sigmoid_table_size - SIGMOID_BOUND;
		sigmoid_table[k] = 1 / (1 + exp(-x));
	}
}

static real FastSigmoid(real x)
{
	if (x > SIGMOID_BOUND) return 1;
	else if (x < -SIGMOID_BOUND) return 0;
	int k = (x + SIGMOID_BOUND) * sigmoid_table_size / SIGMOID_BOUND / 2;
	return sigmoid_table[k];
}

/* Fastly generate a random integer */
static int Rand(unsigned long long &seed)
{
	seed = seed * 25214903917 + 11;
	return (seed >> 16) % neg_table_size;
}

/* Update embeddings */
static void Update(real *vec_u, real *vec_v, real *vec_error, int label)
{
	real x = 0, g;
	for (int c = 0; c != dim; c++) x += vec_u[c] * vec_v[c];
	g = (label - FastSigmoid(x)) * rho;
	for (int c = 0; c != dim; c++) vec_error[c] += g * vec_v[c];
	for (int c = 0; c != dim; c++) vec_v[c] += g * vec_u[c];
}

static void *TrainLINEThread(void *id)
{
	long long u, v, lu, lv, target, label;
	long long count = 0, last_count = 0, curedge;
	unsigned long long seed = (long long)id;
	real *vec_error = (real *)calloc(dim, sizeof(real));

	while (1)
	{
		//judge for exit
		if (count > total_samples / num_threads + 2) break;

		if (count - last_count>10000)
		{
			current_sample_count += count - last_count;
			last_count = count;
			//printf("%cRho: %f  Progress: %.3lf%%", 13, rho, (real)current_sample_count / (real)(total_samples + 1) * 100);
			//fflush(stdout);
			rho = init_rho * (1 - current_sample_count / (real)(total_samples + 1));
			if (rho < init_rho * 0.0001) rho = init_rho * 0.0001;
		}

		curedge = SampleAnEdge(gsl_rng_uniform(gsl_r), gsl_rng_uniform(gsl_r));
		u = edge_source_id[curedge];
		v = edge_target_id[curedge];

		lu = u * dim;
		for (int c = 0; c != dim; c++) vec_error[c] = 0;

		// NEGATIVE SAMPLING
		for (int d = 0; d != num_negative + 1; d++)
		{
			if (d == 0)
			{
				target = v;
				label = 1;
			}
			else
			{
				target = neg_table[Rand(seed)];
				label = 0;
			}
			lv = target * dim;
			if (order == 1) Update(&emb_vertex[lu], &emb_vertex[lv], vec_error, label);
			if (order == 2) Update(&emb_vertex[lu], &emb_context[lv], vec_error, label);
		}
		for (int c = 0; c != dim; c++) emb_vertex[c + lu] += vec_error[c];

		count++;
	}
	free(vec_error);
	pthread_exit(NULL);
}

/* Read network from the training file */
static void VectorReadData(const std::vector<std::string> &input_u, const std::vector<std::string> &input_v, const std::vector<double> &input_w)
{
	char name_v1[MAX_STRING], name_v2[MAX_STRING];
	int vid;
	double weight;

	num_edges = (long long) input_u.size();
	//printf("Number of edges: %lld          \n", num_edges);

	edge_source_id = (int *)malloc(num_edges*sizeof(int));
	edge_target_id = (int *)malloc(num_edges*sizeof(int));
	edge_weight = (double *)malloc(num_edges*sizeof(double));
	if (edge_source_id == NULL || edge_target_id == NULL || edge_weight == NULL)
	{
		Rprintf("Error: memory allocation failed!\n");
        malloc_exit = 1;
        return;
	}

	num_vertices = 0;
	for (int k = 0; k != num_edges; k++)
	{
		strcpy(name_v1, input_u[k].c_str());
		strcpy(name_v2, input_v[k].c_str());
		weight = input_w[k];

		/*if (k % 10000 == 0)
		{
			printf("Reading edges: %.3lf%%%c", k / (double)(num_edges + 1) * 100, 13);
			fflush(stdout);
		}*/

		vid = SearchHashTable(name_v1);
		if (vid == -1) vid = AddVertex(name_v1);
		vertex[vid].degree += weight;
		edge_source_id[k] = vid;

		vid = SearchHashTable(name_v2);
		if (vid == -1) vid = AddVertex(name_v2);
		vertex[vid].degree += weight;
		edge_target_id[k] = vid;

		edge_weight[k] = weight;
	}
	//printf("Number of vertices: %d          \n", num_vertices);
}

static void VectorOutput(std::vector<std::string> &output_vertices, std::vector< std::vector<double> > &output_vectors)
{
	for (int a = 0; a < num_vertices; a++)
	{
		output_vertices.push_back(std::string(vertex[a].name));
		std::vector<double> vec;
		for (int b = 0; b < dim; b++) {
			vec.push_back(emb_vertex[a * dim + b]);
		}
		output_vectors.push_back(vec);
	}
}
/*
static void OutputVectors(std::vector<std::string> &output_vertices, std::vector< std::vector<double> > &output_vectors)
{
	FILE *fo = fopen(embedding_file, "wb");
	fprintf(fo, "%d %d\n", num_vertices, dim);
	for (int a = 0; a < num_vertices; a++)
	{
		fprintf(fo, "%s ", output_vertices[a].c_str());
		if (is_binary) for (int b = 0; b < dim; b++) fwrite(&output_vectors[a][b], sizeof(real), 1, fo);
		else for (int b = 0; b < dim; b++) fprintf(fo, "%lf ", output_vectors[a][b]);
		fprintf(fo, "\n");
	}
	fclose(fo);
}
*/

void TrainLINEMain(const std::vector<std::string> &input_u, const std::vector<std::string> &input_v, const std::vector<double> &input_w, std::vector<std::string> &output_vertices, std::vector< std::vector<double> > &output_vectors,
				  int is_binary_param, int dim_param, int order_param, int num_negative_param, int total_samples_param, float init_rho_param, int num_threads_param) {
	is_binary = is_binary_param;
	dim = dim_param;
	order = order_param;
	num_negative = num_negative_param;
	total_samples = total_samples_param;
	init_rho = init_rho_param;
	num_threads = num_threads_param;
	current_sample_count = 0;
    GetRNGstate();

	total_samples *= 1000000;
	rho = init_rho;
	vertex = (struct ClassVertex *)calloc(max_num_vertices, sizeof(struct ClassVertex));
	long a;
	pthread_t *pt = (pthread_t *)malloc(num_threads * sizeof(pthread_t));

	if (order != 1 && order != 2)
	{
		Rprintf("Error: order should be either 1 or 2!\n");
		return;
	}
	/*printf("--------------------------------\n");
	printf("Binary: %d\n", is_binary);
	printf("Order: %d\n", order);
	printf("Samples: %lldM\n", total_samples / 1000000);
	printf("Negative: %d\n", num_negative);
	printf("Dimension: %d\n", dim);
	printf("Initial rho: %lf\n", init_rho);
	printf("Threads: %d\n", num_threads);
	printf("--------------------------------\n");
    */
	InitHashTable();
	VectorReadData(input_u, input_v, input_w); 
	if (malloc_exit != 0) { return; }
	InitAliasTable();
	if (malloc_exit != 0) { return; }
	InitVector();
	if (malloc_exit != 0) { return; }
	InitNegTable();
	InitSigmoidTable();

	gsl_rng_env_setup();
	gsl_T = gsl_rng_rand48;
	gsl_r = gsl_rng_alloc(gsl_T);
	gsl_rng_set(gsl_r, 314159265);
    
    Rprintf ("generator type: %s\n", gsl_rng_name (gsl_r));
    Rprintf ("seed = %lu\n", gsl_rng_default_seed);
    Rprintf ("first value = %lu\n", gsl_rng_get (gsl_r));
	clock_t start = clock();
	//printf("--------------------------------\n");
	for (a = 0; a < num_threads; a++) pthread_create(&pt[a], NULL, TrainLINEThread, (void *)a);
	for (a = 0; a < num_threads; a++) pthread_join(pt[a], NULL);
	//printf("\n");
    PutRNGstate();
	clock_t finish = clock();
	//printf("Total time: %lf\n", (double)(finish - start) / CLOCKS_PER_SEC);

	VectorOutput(output_vertices, output_vectors); //Output();
}
/*
static void ReadVectors(std::vector<std::string> &input_u, std::vector<std::string> &input_v, std::vector<double> &input_w) {
        FILE *fin;
        char name_v1[MAX_STRING], name_v2[MAX_STRING], str[2 * MAX_STRING + 10000];
        double weight;

        fin = fopen(network_file, "rb");
        if (fin == NULL)
        {
                printf("ERROR: network file not found!\n");
                exit(1);
        }

        num_edges = 0;
        while (fgets(str, sizeof(str), fin)) num_edges++;
        fclose(fin);

        fin = fopen(network_file, "rb");
        for (int k = 0; k != num_edges; k++)
        {
                fscanf(fin, "%s %s %lf", name_v1, name_v2, &weight);
                input_u.push_back(std::string(name_v1));
                input_v.push_back(std::string(name_v2));
                input_w.push_back(weight);
        }
        fclose(fin);
}

static int ArgPos(char *str, int argc, char **argv) {
	int a;
	for (a = 1; a < argc; a++) if (!strcmp(str, argv[a])) {
		if (a == argc - 1) {
			printf("Argument missing for %s\n", str);
			exit(1);
		}
		return a;
	}
	return -1;
}*/
