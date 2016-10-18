#include <math.h>
#include <float.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <cstring>
#include <time.h>
#include "vptree.h"
#include "sasne.h"

#include "satree.h"
#include "LargeVis.h"

using namespace std;

#define BILLION 1000000000L

SASNE::SASNE() {

}

SASNE::~SASNE() {
    if (tree != NULL) delete tree;
}

void SASNE::run(double* X, int N, int D, /*change*/double* Y, int no_dims, double perplexity, double theta,
               unsigned int bins, int p_method, int rand_seed, bool skip_random_init, int max_iter, int stop_lying_iter, 
               int mom_switch_iter) { 

    // Set random seed
    if (skip_random_init != true) {
      if(rand_seed >= 0) {
          printf("Using random seed: %d\n", rand_seed);
          srand((unsigned int) rand_seed);
      } else {
          printf("Using current time as random seed...\n");
          srand(time(NULL));
      }
    }

    // Determine whether we are using an exact algorithm
    if(N - 1 < 3 * perplexity) { printf("Perplexity too large for the number of data points!\n"); exit(1); }
    printf("Using no_dims = %d, perplexity = %f, bins = %d, p_method = %d and theta = %f\n", no_dims, perplexity, bins, p_method, theta);
    bool exact = (theta == .0) ? true : false;

    // Set learning parameters
    float total_time = .0;
    clock_t start, end;
	double momentum = .5, final_momentum = .8;
	double eta = 200.0;

    // Allocate some memory
    double* dY    = (double*) malloc(N * no_dims * sizeof(double));
    double* uY    = (double*) malloc(N * no_dims * sizeof(double));
    double* gains = (double*) malloc(N * no_dims * sizeof(double));
    if(dY == NULL || uY == NULL || gains == NULL) { printf("Memory allocation failed!\n"); exit(1); }
    for(int i = 0; i < N * no_dims; i++)    uY[i] =  .0;
    for(int i = 0; i < N * no_dims; i++) gains[i] = 1.0;

    // Normalize input data (to prevent numerical problems)
    printf("Computing input similarities...\n");
    start = clock();

    double* P=NULL;                      // conditional probability P.
    unsigned long long* row_P = NULL;        
    unsigned long long* col_P = NULL;
    double* val_P = NULL;


    if(exact) { 
        // Compute similarities
        printf("Exact?");
    }

    else {  

        FILE *flog = fopen("log.txt", "wb");

        struct timespec start_p, end_p;

        clock_gettime(CLOCK_MONOTONIC, &start_p);

        // Using LargeVis
        if (p_method != 1) {
            printf("P Method: Construct_KNN\n");

            long long if_embed = 1, out_dim = -1, n_samples = -1, n_threads = 1, n_negative = -1, n_neighbors = -1, n_trees = -1, n_propagation = -1;
            real alpha = -1, n_gamma = -1;

            LargeVis p_model;
            p_model.load_from_data(X, N, D);
            p_model.run(out_dim, n_threads, n_samples, n_propagation, alpha, n_trees, n_negative, n_neighbors, n_gamma, perplexity);
            p_model.get_result(&row_P, &col_P, &val_P);

            symmetrizeMatrix(&row_P, &col_P, &val_P, N);

            int need_log = 0;
            if (need_log){
                FILE* fp_saved = fopen("saved_symmetrized.txt", "w+");
                char temp_str[100] = "";

                int idx = 0;
                for(int n = 0; n < N; n++) {
                    for(int i = row_P[n]; i < row_P[n + 1]; i++) {
                        ++idx;

                        sprintf(temp_str, "%lld %lld %f\n", row_P[n], col_P[idx], val_P[idx]);
                        fwrite(temp_str, strlen(temp_str), 1, fp_saved);
                    }
                }

                fclose(fp_saved);
            }
            
            double sum_P = .0;
            for(int i = 0; i < row_P[N]; i++) {
                sum_P += val_P[i];
            }
            for(int i = 0; i < row_P[N]; i++) {
                val_P[i] /= sum_P;
            }

        }
        else 
        {
            printf("P Method: VP Tree\n");

            // Compute asymmetric pairwise input similarities
            computeGaussianPerplexity(X, N, D, &row_P, &col_P, &val_P, perplexity, (int) (3 * perplexity));

            // Symmetrize input similarities
            symmetrizeMatrix(&row_P, &col_P, &val_P, N);

            double sum_P = .0;
            for(int i = 0; i < row_P[N]; i++) {
                sum_P += val_P[i];
            }
            for(int i = 0; i < row_P[N]; i++) {
                val_P[i] /= sum_P;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &end_p);

        double elapsed  = (double)(end_p.tv_sec - start_p.tv_sec) + (double)(end_p.tv_nsec - start_p.tv_nsec)/BILLION;

        printf("P training time: %.2f seconds!\n", elapsed);
    }
    end = clock();
    //printf("end: %.2f, CLOCKS_PER_SEC: %.2f, elapsed: %.2f", end, CLOCKS_PER_SEC, (float)(end - start)/CLOCKS_PER_SEC);

    // Lie about the P-values
    if(exact) { for(int i = 0; i < N * N; i++)        P[i] *= 12.0; }
    else {      for(int i = 0; i < row_P[N]; i++) val_P[i] *= 12.0; }
    
    if (skip_random_init != true) {
        for(int i = 0; i < N * no_dims; i++) {
            Y[i] = rand() % bins;
        }
    }

	// Perform main training loop
    if(exact) printf("Input similarities computed in %4.2f seconds!\nLearning embedding...\n", (float) (end - start) / CLOCKS_PER_SEC);
    else printf("Input similarities computed in %4.2f seconds (sparsity = %f)!\nLearning embedding...\n", (float) (end - start) / CLOCKS_PER_SEC, (double) row_P[N] / ((double) N * (double) N));

    double beta = bins * bins * 1e3;
    start = clock();

    tree = NULL;
	
	for(int iter = 0; iter < max_iter; iter++) {
        if(exact) computeExactGradient(P, Y, N, no_dims, dY);
        else computeGradient(P, row_P, col_P, val_P, Y, N, no_dims, dY, theta, beta, bins, iter);

        // Update gains
        for(int i = 0; i < N * no_dims; i++) gains[i] = (sign(dY[i]) != sign(uY[i])) ? (gains[i] + .2) : (gains[i] * .8);
        for(int i = 0; i < N * no_dims; i++) if(gains[i] < .01) gains[i] = .01;

        // Perform gradient update (with momentum and gains)
        for(int i = 0; i < N * no_dims; i++) uY[i] = momentum * uY[i] - eta * gains[i] * dY[i];
		for(int i = 0; i < N * no_dims; i++)  Y[i] = Y[i] + uY[i];

        beta = minmax(Y, N, no_dims, beta, bins, iter);

        // Stop lying about the P-values after a while, and switch momentum
        if(iter == stop_lying_iter) {
            if(exact) { for(int i = 0; i < N * N; i++)        P[i] /= 12.0; }
            else      { for(int i = 0; i < row_P[N]; i++) val_P[i] /= 12.0; }
        }
        if(iter == mom_switch_iter) momentum = final_momentum;

        // Print out progress
        if (iter > 0 && (iter % 50 == 0 || iter == max_iter - 1)) {
            end = clock();
            double C = .0;
            if(exact) C = evaluateError(P, Y, N, no_dims);
            else      C = evaluateError(row_P, col_P, val_P, Y, N, no_dims, theta, beta, bins, iter);  // doing approximate computation here!
            if(iter == 0)
                printf("Iteration %d: error is %f\n", iter + 1, C);
            else {
                total_time += (float) (end - start) / CLOCKS_PER_SEC;
                printf("Iteration %d: error is %f (50 iterations in %4.2f seconds)\n", iter, C, (float) (end - start) / CLOCKS_PER_SEC);
            }
			start = clock();
        }
    }

    end = clock(); 
    total_time += (float) (end - start) / CLOCKS_PER_SEC;

    // Clean up memory
    free(dY);
    free(uY);
    free(gains);
    if(exact) free(P);
    else {
        free(row_P); row_P = NULL;
        free(col_P); col_P = NULL;
        free(val_P); val_P = NULL;
    }
    printf("Fitting performed in %4.2f seconds.\n", total_time);
}

void SASNE::computeGradient(double* P, unsigned long long* inp_row_P, unsigned long long* inp_col_P, double* inp_val_P, /*change*/double* Y, int N, int D, double* dC, 
    double theta, double beta, unsigned int bins, int iter_cnt){ 
    clock_t start, end;

    // Construct space-partitioning tree on current map
    start = clock();
    if (tree == NULL)
        // tree = new SATree(D, Y, N, ROOT_PIXEL_WIDTH, 0, iter_cnt);
        tree = new SATree(D, Y, N, bins, 0, iter_cnt);
    else {
        tree->clean(iter_cnt);
        tree->fill(N, iter_cnt);
    }
    end = clock();

    // Compute all terms required for t-SNE gradient
    start = clock();
    double sum_Q = .0;
    double* pos_f = (double*) calloc(N * D, sizeof(double));
    double* neg_f = (double*) calloc(N * D, sizeof(double));
    if(pos_f == NULL || neg_f == NULL) { printf("Memory allocation failed!\n"); exit(1); }
    tree->computeEdgeForces(inp_row_P, inp_col_P, inp_val_P, N, pos_f, beta);
    for(int n = 0; n < N; n++) tree->computeNonEdgeForces(n, theta, neg_f + n * D, &sum_Q, beta, iter_cnt);

    // Compute final t-SNE gradient
    for(int i = 0; i < N * D; i++) {
        dC[i] = pos_f[i] - (neg_f[i] / sum_Q);
    }
    free(pos_f);
    free(neg_f);
    end = clock();
    // printf("Computing gradient computed in %4.2f seconds!\n", (float) (end - start) / CLOCKS_PER_SEC);
    
}

// Compute gradient of the t-SNE cost function (exact)
void SASNE::computeExactGradient(double* P, /*change*/double* Y, int N, int D, double* dC) {

	// Make sure the current gradient contains zeros
	for(int i = 0; i < N * D; i++) dC[i] = 0.0;

    // Compute the squared Euclidean distance matrix
    double* DD = (double*) malloc(N * N * sizeof(double));
    if(DD == NULL) { printf("Memory allocation failed!\n"); exit(1); }
    computeSquaredEuclideanDistance(Y, N, D, DD);

    // Compute Q-matrix and normalization sum
    double* Q    = (double*) malloc(N * N * sizeof(double));
    if(Q == NULL) { printf("Memory allocation failed!\n"); exit(1); }
    double sum_Q = .0;
    int nN = 0;
    for(int n = 0; n < N; n++) {
    	for(int m = 0; m < N; m++) {
            if(n != m) {
                Q[nN + m] = 1 / (1 + DD[nN + m]);
                sum_Q += Q[nN + m];
            }
        }
        nN += N;
    }

	// Perform the computation of the gradient
    nN = 0;
    int nD = 0;
	for(int n = 0; n < N; n++) {
        int mD = 0;
    	for(int m = 0; m < N; m++) {
            if(n != m) {
                double mult = (P[nN + m] - (Q[nN + m] / sum_Q)) * Q[nN + m];
                for(int d = 0; d < D; d++) {
                    dC[nD + d] += (Y[nD + d] - Y[mD + d]) * mult;
                }
            }
            mD += D;
		}
        nN += N;
        nD += D;
	}

    // Free memory
    free(DD); DD = NULL;
    free(Q);  Q  = NULL;
}


// Evaluate t-SNE cost function (exactly)
double SASNE::evaluateError(double* P, /*change*/double* Y, int N, int D) {

    // Compute the squared Euclidean distance matrix
    double* DD = (double*) malloc(N * N * sizeof(double));
    double* Q = (double*) malloc(N * N * sizeof(double));
    if(DD == NULL || Q == NULL) { printf("Memory allocation failed!\n"); exit(1); }
    computeSquaredEuclideanDistance(Y, N, D, DD);

    // Compute Q-matrix and normalization sum
    int nN = 0;
    double sum_Q = DBL_MIN;
    for(int n = 0; n < N; n++) {
    	for(int m = 0; m < N; m++) {
            if(n != m) {
                Q[nN + m] = 1 / (1 + DD[nN + m]);
                sum_Q += Q[nN + m];
            }
            else Q[nN + m] = DBL_MIN;
        }
        nN += N;
    }
    for(int i = 0; i < N * N; i++) Q[i] /= sum_Q;

    // Sum t-SNE error
    double C = .0;
	for(int n = 0; n < N * N; n++) {
        C += P[n] * log((P[n] + FLT_MIN) / (Q[n] + FLT_MIN));
	}

    // Clean up memory
    free(DD);
    free(Q);
	return C;
}

// Evaluate t-SNE cost function (approximately)
double SASNE::evaluateError(unsigned long long* row_P, unsigned long long* col_P, double* val_P, /*change*/double* Y, int N, int D, double theta, double beta, unsigned int bins, int iter_cnt)
{

    // Get estimate of normalization term
    SATree* dtree = new SATree(D, Y, N, bins, 0, iter_cnt);
    double* buff = (double*) calloc(D, sizeof(double));
    double sum_Q = .0;
    for(int n = 0; n < N; n++) dtree->computeNonEdgeForces(n, theta, buff, &sum_Q, beta, iter_cnt);

    // Loop over all edges to compute t-SNE error
    int ind1, ind2;
    double C = .0, Q;
    for(int n = 0; n < N; n++) {
        ind1 = n * D;
        for(int i = row_P[n]; i < row_P[n + 1]; i++) {
            Q = .0;
            ind2 = col_P[i] * D;
            for(int d = 0; d < D; d++) buff[d]  = Y[ind1 + d];
            for(int d = 0; d < D; d++) buff[d] -= Y[ind2 + d];
            for(int d = 0; d < D; d++) Q += buff[d] * buff[d];
            // Q = (1.0 / (1.0 + Q)) / sum_Q;
            Q = (beta / (beta + Q)) / sum_Q;
            C += val_P[i] * log((val_P[i] + FLT_MIN) / (Q + FLT_MIN));

        }
    }

    // Clean up memory
    free(buff);
    delete dtree;
    return C;
}


// Compute input similarities with a fixed perplexity
void SASNE::computeGaussianPerplexity(/*change*/double* X, int N, int D, double* P, double perplexity) {    

	// Compute the squared Euclidean distance matrixgoog
	double* DD = (double*) malloc(N * N * sizeof(double)); // symmetric metrix
    if(DD == NULL) { printf("Memory allocation failed!\n"); exit(1); }
	computeSquaredEuclideanDistance(X, N, D, DD);          // time complexity: O(N^2)

	// Compute the Gaussian kernel row by row
    int nN = 0;
	for(int n = 0; n < N; n++) {

		// Initialize some variables
		bool found = false;
		double beta = 1.0;
		double min_beta = -DBL_MAX;
		double max_beta =  DBL_MAX;
		double tol = 1e-5;
        double sum_P;

		// Iterate until we found a good perplexity
		int iter = 0;
		while(!found && iter < 200) {

			// Compute Gaussian kernel row
			for(int m = 0; m < N; m++) P[nN + m] = exp(-beta * DD[nN + m]);
			P[nN + n] = DBL_MIN;

			// Compute entropy of current row
			sum_P = DBL_MIN;
			for(int m = 0; m < N; m++) sum_P += P[nN + m];
			double H = 0.0;
			for(int m = 0; m < N; m++) H += beta * (DD[nN + m] * P[nN + m]);
			H = (H / sum_P) + log(sum_P);

			// Evaluate whether the entropy is within the tolerance level
			double Hdiff = H - log(perplexity);
			if(Hdiff < tol && -Hdiff < tol) {
				found = true;
			}
			else {
				if(Hdiff > 0) {
					min_beta = beta;
					if(max_beta == DBL_MAX || max_beta == -DBL_MAX)
						beta *= 2.0;
					else
						beta = (beta + max_beta) / 2.0;
				}
				else {
					max_beta = beta;
					if(min_beta == -DBL_MAX || min_beta == DBL_MAX)
						beta /= 2.0;
					else
						beta = (beta + min_beta) / 2.0;
				}
			}

			// Update iteration counter
			iter++;
		}

		// Row normalize P
		for(int m = 0; m < N; m++) P[nN + m] /= sum_P;
        nN += N;
	}

	// Clean up memory
	free(DD); DD = NULL;
}


// Compute input similarities with a fixed perplexity using ball trees (this function allocates memory another function should free)
void SASNE::computeGaussianPerplexity(double* X, int N, int D, unsigned long long** _row_P, unsigned long long** _col_P, double** _val_P, double perplexity, int K) {

    if(perplexity > K) printf("Perplexity should be lower than K!\n");


    // Allocate the memory we need
    *_row_P = (unsigned long long*)    malloc((N + 1) * sizeof(unsigned long long));
    *_col_P = (unsigned long long*)    calloc(N * K, sizeof(unsigned long long));
    *_val_P = (double*) calloc(N * K, sizeof(double));
    if(*_row_P == NULL || *_col_P == NULL || *_val_P == NULL) { printf("Memory allocation failed!\n"); exit(1); }
    unsigned long long* row_P = *_row_P;
    unsigned long long* col_P = *_col_P;
    double* val_P = *_val_P;
    double* cur_P = (double*) malloc((N - 1) * sizeof(double));
    if(cur_P == NULL) { printf("Memory allocation failed!\n"); exit(1); }
    row_P[0] = 0;
    for(int n = 0; n < N; n++) row_P[n + 1] = row_P[n] + (unsigned long long) K;

    // Build ball tree on data set(Vantage-point tree) --> time complexity: O(uNlogN)
    VpTree<DataPoint, euclidean_distance>* tree = new VpTree<DataPoint, euclidean_distance>();
    vector<DataPoint> obj_X(N, DataPoint(D, -1, X));
    for(int n = 0; n < N; n++) obj_X[n] = DataPoint(D, n, X + n * D);
    tree->create(obj_X);

    // Loop over all points to find nearest neighbors
    printf("Building tree...\n");
    vector<DataPoint> indices;
    vector<double> distances;
    for(int n = 0; n < N; n++) {

        if(n % 10000 == 0) printf(" - point %d of %d\n", n, N);

        // Find nearest neighbors
        indices.clear();
        distances.clear();
        tree->search(obj_X[n], K + 1, &indices, &distances);

        // Initialize some variables for binary search
        bool found = false;
        double beta = 1.0;
        double min_beta = -DBL_MAX;
        double max_beta =  DBL_MAX;
        double tol = 1e-5;

        // Iterate until we found a good perplexity
        int iter = 0; double sum_P;
        while(!found && iter < 200) {

            // Compute Gaussian kernel row
            for(int m = 0; m < K; m++) cur_P[m] = exp(-beta * distances[m + 1] * distances[m + 1]);

            // Compute entropy of current row
            sum_P = DBL_MIN;
            for(int m = 0; m < K; m++) sum_P += cur_P[m];
            double H = .0;
            for(int m = 0; m < K; m++) H += beta * (distances[m + 1] * distances[m + 1] * cur_P[m]);
            H = (H / sum_P) + log(sum_P);

            // Evaluate whether the entropy is within the tolerance level
            double Hdiff = H - log(perplexity);
            if(Hdiff < tol && -Hdiff < tol) {
                found = true;
            }
            else {
                if(Hdiff > 0) {
                    min_beta = beta;
                    if(max_beta == DBL_MAX || max_beta == -DBL_MAX)
                        beta *= 2.0;
                    else
                        beta = (beta + max_beta) / 2.0;
                }
                else {
                    max_beta = beta;
                    if(min_beta == -DBL_MAX || min_beta == DBL_MAX)
                        beta /= 2.0;
                    else
                        beta = (beta + min_beta) / 2.0;
                }
            }

            // Update iteration counter
            iter++;
        }

        // Row-normalize current row of P and store in matrix
        for(unsigned int m = 0; m < K; m++) cur_P[m] /= sum_P;
        for(unsigned int m = 0; m < K; m++) {
            col_P[row_P[n] + m] = (unsigned int) indices[m + 1].index();
            val_P[row_P[n] + m] = cur_P[m];
        }
    }

    // Clean up memory
    obj_X.clear();
    free(cur_P);
    delete tree;
}


// Symmetrizes a sparse matrix
void SASNE::symmetrizeMatrix(unsigned long long** _row_P, unsigned long long** _col_P, double** _val_P, int N) {

    // Get sparse matrix
    unsigned long long* row_P = *_row_P;
    unsigned long long* col_P = *_col_P;
    double* val_P = *_val_P;

    // Count number of elements and row counts of symmetric matrix
    int* row_counts = (int*) calloc(N, sizeof(int));
    if(row_counts == NULL) { printf("Memory allocation failed!\n"); exit(1); }
    for(int n = 0; n < N; n++) {
        for(int i = row_P[n]; i < row_P[n + 1]; i++) {

            // Check whether element (col_P[i], n) is present
            bool present = false;
            for(int m = row_P[col_P[i]]; m < row_P[col_P[i] + 1]; m++) {
                if(col_P[m] == n) present = true;
            }
            if(present) row_counts[n]++;
            else {
                row_counts[n]++;
                row_counts[col_P[i]]++;
            }
        }
    }
    int no_elem = 0;
    for(int n = 0; n < N; n++) no_elem += row_counts[n];

    // Allocate memory for symmetrized matrix
    unsigned long long* sym_row_P = (unsigned long long*) malloc((N + 1) * sizeof(unsigned long long));
    unsigned long long* sym_col_P = (unsigned long long*) malloc(no_elem * sizeof(unsigned long long));
    double* sym_val_P = (double*) malloc(no_elem * sizeof(double));
    if(sym_row_P == NULL || sym_col_P == NULL || sym_val_P == NULL) { printf("Memory allocation failed!\n"); exit(1); }

    // Construct new row indices for symmetric matrix
    sym_row_P[0] = 0;
    for(int n = 0; n < N; n++) sym_row_P[n + 1] = sym_row_P[n] + (unsigned int) row_counts[n];

    // Fill the result matrix
    int* offset = (int*) calloc(N, sizeof(int));
    if(offset == NULL) { printf("Memory allocation failed!\n"); exit(1); }
    for(int n = 0; n < N; n++) {
        for(unsigned int i = row_P[n]; i < row_P[n + 1]; i++) {                                  // considering element(n, col_P[i])

            // Check whether element (col_P[i], n) is present
            bool present = false;
            for(unsigned int m = row_P[col_P[i]]; m < row_P[col_P[i] + 1]; m++) {
                if(col_P[m] == n) {
                    present = true;
                    if(n <= col_P[i]) {                                                 // make sure we do not add elements twice
                        sym_col_P[sym_row_P[n]        + offset[n]]        = col_P[i];
                        sym_col_P[sym_row_P[col_P[i]] + offset[col_P[i]]] = n;
                        sym_val_P[sym_row_P[n]        + offset[n]]        = val_P[i] + val_P[m];
                        sym_val_P[sym_row_P[col_P[i]] + offset[col_P[i]]] = val_P[i] + val_P[m];
                    }
                }
            }

            // If (col_P[i], n) is not present, there is no addition involved
            if(!present) {
                sym_col_P[sym_row_P[n]        + offset[n]]        = col_P[i];
                sym_col_P[sym_row_P[col_P[i]] + offset[col_P[i]]] = n;
                sym_val_P[sym_row_P[n]        + offset[n]]        = val_P[i];
                sym_val_P[sym_row_P[col_P[i]] + offset[col_P[i]]] = val_P[i];
            }

            // Update offsets
            if(!present || (present && n <= col_P[i])) {
                offset[n]++;
                if(col_P[i] != n) offset[col_P[i]]++;
            }
        }
    }

    // Divide the result by two
    for(int i = 0; i < no_elem; i++) sym_val_P[i] /= 2.0;

    // Return symmetrized matrices
    free(*_row_P); *_row_P = sym_row_P;
    free(*_col_P); *_col_P = sym_col_P;
    free(*_val_P); *_val_P = sym_val_P;

    // Free up some memery
    free(offset); offset = NULL;
    free(row_counts); row_counts  = NULL;
}

// Compute squared Euclidean distance matrix
void SASNE::computeSquaredEuclideanDistance(double* X, int N, int D, double* DD) {
    const double* XnD = X;
    for(int n = 0; n < N; ++n, XnD += D) {
        const double* XmD = XnD + D;
        double* curr_elem = &DD[n*N + n];
        *curr_elem = 0.0;                       // 초기화를 하면서 진행? --> matrix 생성시 초기화 하는 것이 좋음
        double* curr_elem_sym = curr_elem + N;
        for(int m = n + 1; m < N; ++m, XmD+=D, curr_elem_sym+=N) {
            *(++curr_elem) = 0.0;
            for(int d = 0; d < D; ++d) {
                *curr_elem += (XnD[d] - XmD[d]) * (XnD[d] - XmD[d]);    // compute distance and accumulate.
            }
            *curr_elem_sym = *curr_elem;
        }
    }
}


// Makes data zero-mean
void SASNE::zeroMean(double* X, int N, int D) {
	// Compute data mean
	double* mean = (double*) calloc(D, sizeof(double));
    if(mean == NULL) { printf("Memory allocation failed!\n"); exit(1); }
    int nD = 0;
	for(int n = 0; n < N; n++) {
		for(int d = 0; d < D; d++) {
			mean[d] += X[nD + d];
		}
        nD += D;
	}
	for(int d = 0; d < D; d++) {
		mean[d] /= (double) N;
	}

	// Subtract data mean
    nD = 0;
	for(int n = 0; n < N; n++) {
		for(int d = 0; d < D; d++) {
			X[nD + d] -= mean[d];
		}
        nD += D;
	}
    free(mean); mean = NULL;
}

double SASNE::minmax(double* X, int N, int D, double beta, unsigned int bins, int iter_cnt) {
    
    // Compute data min, max
    double ran, xran, yran; 
    double* min = (double*) calloc(D, sizeof(double));
    double* max = (double*) calloc(D, sizeof(double));
    if(min == NULL || max == NULL) { printf("Memory allocation failed!\n"); exit(1); }

    for (int d = 0 ; d < D; d++) {
        min[d] = INT_MAX;
        max[d] = INT_MIN;
    }

    int nD = 0;
    for(int n = 0; n < N; n++) {
        for(int d = 0; d < D; d++) {
            if (min[d] > X[nD + d]) {
                min[d] = X[nD + d];
            }
        }
        nD += D;
    }

    nD = 0;
    for(int n = 0; n < N; n++) {
        for(int d = 0; d < D; d++) {
            X[nD + d] -= min[d];
        }
        nD += D;
    }

    nD = 0;
    for(int n = 0; n < N; n++) {
        for(int d = 0; d < D; d++) {
            if (max[d] < X[nD + d]) {
                max[d] = X[nD + d];
            }
        }
        nD += D;
    }

    // xran = float(ROOT_PIXEL_WIDTH-1) / (max[0]);
    // yran = float(ROOT_PIXEL_WIDTH-1) / (max[1]);
    xran = float(bins-1) / (max[0]);
    yran = float(bins-1) / (max[1]);

    //if (iter_cnt < 10)
        //printf("max[0]: %f, max[1]: %f\n", max[0], max[1]);

    ran = (xran < yran) ? xran : yran;
    beta = beta * pow(ran, 2);

    // Subtract min, max
    nD = 0;
    for(int n = 0; n < N; n++) {
        for (int d = 0; d < D; d++) {
            
            if (0 == d) {
                X[nD + d] = floor (X[nD + d] * xran) ;
            }
            else{
                X[nD + d] = floor (X[nD + d] * yran) ;
            }
        }
        nD += D;
    }
    nD = 0;

    free(min); free(max); min = NULL; max = NULL;
    return beta;
}


// Generates a Gaussian random number
double SASNE::randn() {
	double x, y, radius;
	do {
		x = 2 * (rand() / ((double) RAND_MAX + 1)) - 1;
		y = 2 * (rand() / ((double) RAND_MAX + 1)) - 1;
		radius = (x * x) + (y * y);
	} while((radius >= 1.0) || (radius == 0.0));
	radius = sqrt(-2 * log(radius) / radius);
	x *= radius;
	y *= radius;
	return x;
}

// Function that loads data from a t-SNE file
// Note: this function does a malloc that should be freed elsewhere
bool SASNE::load_data(double** data, int* n, int* d, int* no_dims, double* theta, double* perplexity, unsigned int* bins, int* p_method, int* rand_seed) {

    // Open file, read first 2 integers, allocate memory, and read the data
    FILE *h;
    size_t res = -1;
    if((h = fopen("data.dat", "r+b")) == NULL) {
        printf("Error: could not open data file.\n");
        return false;
    }
    res = fread(n, sizeof(int), 1, h);                                            // number of datapoints
    res = fread(d, sizeof(int), 1, h);                                            // original dimensionality
    res = fread(theta, sizeof(double), 1, h);                                     // gradient accuracy
    res = fread(perplexity, sizeof(double), 1, h);                                // perplexity
    res = fread(no_dims, sizeof(int), 1, h);                                      // output dimensionality
    res = fread(p_method, sizeof(int), 1, h);                                     // 1: vp-tree, 0: construct-knn
    res = fread(bins, sizeof(unsigned int), 1, h);

    *data = (double*) malloc(*d * *n * sizeof(double));
    if(*data == NULL) { printf("Memory allocation failed!\n"); exit(1); }
    res = fread(*data, sizeof(double), *n * *d, h);                               // the data

    if(!feof(h)) res = fread(rand_seed, sizeof(int), 1, h);                       // random seed
    fclose(h);
    printf("Read the %i x %i data matrix successfully!\n", *n, *d);


    return true;
}


// Function that saves map to a t-SNE file
void SASNE::save_data(double* data, int* landmarks, double* costs, int n, int d) {

	// Open file, write first 2 integers and then the data
	FILE *h;
	if((h = fopen("result.dat", "w+b")) == NULL) {
		printf("Error: could not open data file.\n");
		return;
	}
	fwrite(&n, sizeof(int), 1, h);
	fwrite(&d, sizeof(int), 1, h);
    fwrite(data, sizeof(double), n * d, h);
	fwrite(landmarks, sizeof(int), n, h);
    fwrite(costs, sizeof(double), n, h);
    fclose(h);
	printf("Wrote the %i x %i data matrix successfully!\n", n, d);
}


// Function that runs the Barnes-Hut implementation of t-SNE
int main() {

    // Define some variables
    int     origN;                  // original input data size?
    int     N;                      // input data size
    int     D;                      // dimensionality(high dimension??)
    int     no_dims;                // row dimension dimensionality
    int*    landmarks;              // array of landmarks
	double  perc_landmarks;         // ??
    double  perplexity;             // (u), perplexity of conditional distribution
    double  theta;                  // gradient accuracy
    double* data;                   // the data
    unsigned int bins;
    int     p_method;
    int rand_seed = 30;             // random seed
    SASNE* sasne = new SASNE();

    // Read the parameters and the dataset
	if(sasne->load_data(&data, &origN, &D, &no_dims, &theta, &perplexity, &bins, &p_method, &rand_seed)) {
		// Make dummy landmarks
        N = origN;

        int* landmarks = (int*) malloc(N * sizeof(int));        // 모든 점을 Landmark로 설정. Paper에 모든 점을 사용한다고 되어 있음.
        if(landmarks == NULL) { printf("Memory allocation failed!\n"); exit(1); }
        for(int n = 0; n < N; n++) landmarks[n] = n;            // Set dummy landmarks

        double* Y = (double*) malloc(N * no_dims * sizeof(double)); 
		double* costs = (double*) calloc(N, sizeof(double));          // 모든 점에 대한 cost
        if(Y == NULL || costs == NULL) { printf("Memory allocation failed!\n"); exit(1); }
        
        sasne->run(data, N, D, Y, no_dims, perplexity, theta, bins, p_method, rand_seed, false);
		sasne->save_data(Y, landmarks, costs, N, no_dims);

        // Clean up the memory
        if (data != NULL){
            free(data); data = NULL;    
        }
		free(Y); Y = NULL;
		free(costs); costs = NULL;
		free(landmarks); landmarks = NULL;
    }
    delete(sasne);
}
