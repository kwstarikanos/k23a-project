#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>

#include "../include/logreg.h"
#include "../include/queue.h"
#include "../include/colours.h"
#include "../include/job_scheduler.h"

extern JobScheduler js;

LogReg *lr_new(int weights_len, float learning_rate) {
    unsigned int seed = 12345;
    LogReg *out = malloc(sizeof(*out));
    out->weights_len = weights_len;
    out->weights = malloc(weights_len * sizeof(float));
    for (int i = 0; i < weights_len; i++)
        out->weights[i] = ((float) rand_r(&seed)) / RAND_MAX;
    out->bias = ((float) rand_r(&seed)) / RAND_MAX;
    out->learning_rate = learning_rate;
    return out;
}

LogReg *lr_new_from_file(FILE *fp, bool *bow) {
    char buf[32];
    LogReg *model = lr_new(0, 0);
    fgets(buf, 32, fp);
    *bow = strcmp(buf, "bow") != 0;
    fgets(buf, 32, fp);
    model->learning_rate = atof(buf);
    fgets(buf, 32, fp);
    model->bias = atof(buf);
    fgets(buf, 32, fp);
    model->weights_len = atof(buf);
    model->weights = malloc(model->weights_len * sizeof(float));
    for (int i = 0; i < model->weights_len; ++i) {
        fgets(buf, 32, fp);
        model->weights[i] = atof(buf);
    }
    return model;
}

void lr_cpy(LogReg **dst, LogReg *src) {
    *dst = lr_new(0, 0);
    (*dst)->learning_rate = src->learning_rate;
    (*dst)->bias = src->bias;
    (*dst)->weights_len = src->weights_len;
    (*dst)->weights = malloc(src->weights_len * sizeof(float));
    for (int i = 0; i < src->weights_len; ++i) {
        (*dst)->weights[i] = src->weights[i];
    }
}

void lr_export_model(LogReg *reg, bool bow, char *path) {
    char filepath[100];
    snprintf(filepath, 100, "%s/%s", path, "model.csv");
    FILE *fp = fopen(filepath, "w+");
    assert(fp != NULL);
    fprintf(fp, "%s\n%f\n%f\n%d\n", bow ? "bow" : "tfidf", reg->learning_rate, reg->bias, reg->weights_len);
    for (int i = 0; i < reg->weights_len; ++i) {
        fprintf(fp, "%f\n", reg->weights[i]);
    }
    fclose(fp);
}

void lr_free(LogReg *reg) {
    free(reg->weights);
    free(reg);
}

float lr_sigmoid(float x) { return exp(x) / (1 + exp(x)); }

float lr_loss(float p, bool y) { return -log((y ? p : 1 - p)); }

float lr_predict_one(LogReg *reg, float *X) {
    float lin_sum = 0;
    float p;
    int i;
    for (i = 0; i < reg->weights_len; i++) {
        lin_sum += reg->weights[i] * X[i];
    }
    p = lr_sigmoid(lin_sum + reg->bias);
    return p;
}

float *lr_predict(LogReg *reg, float *Xs, int batch_sz) {
    float *Ps = malloc(sizeof(Ps) * batch_sz);
    for (int i = 0; i < batch_sz; i++)
        Ps[i] = lr_predict_one(reg, &Xs[i * reg->weights_len]);
    return Ps;
}

pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
#define LOCK_ pthread_mutex_lock(&mtx)
#define UNLOCK_ pthread_mutex_unlock(&mtx)

void *loop(Job job) {
    LogReg *reg;
    float *Xs = NULL, *Ps = NULL, *Deltas = NULL;
    int i = 0, *Ys = NULL;
    js_get_args(job, &reg, &Deltas, &Xs, &Ys, &Ps, &i, NULL);
    //printf(CYAN"Thread [%ld] job %lld i=%d\n"RESET, pthread_self(), js_get_job_id(job), i);
    //float *Deltas = malloc((reg->weights_len + 1) * sizeof(float));
    //memset(Deltas, 0, (reg->weights_len + 1) * sizeof(float));
    int j;
    for (j = 0; j < reg->weights_len; j++) {
        /* j is inner loop for cache efficiency */

        LOCK_;
        Deltas[j] += reg->learning_rate * (Ps[i] - Ys[i]) * Xs[i * reg->weights_len + j];
        UNLOCK_;
    }
    /* Delta for the bias */

    LOCK_;
    Deltas[j] += reg->learning_rate * (Ps[i] - Ys[i]);
    UNLOCK_;

    //printf(CYAN"Thread [%ld] job %lld calculated deltas\n"RESET, pthread_self(), js_get_job_id(job));
    return Deltas;
}

float lr_train(LogReg *reg, float *Xs, int *Ys, int batch_sz) {
    float *Ps = lr_predict(reg, Xs, batch_sz);

    /* calculate the Deltas */
    float *Deltas = malloc((reg->weights_len + 1) * sizeof(float));
    memset(Deltas, 0, (reg->weights_len + 1) * sizeof(float));

    /*Επαναλαμβάνεται μέχρι εξάντηλησης παρατηρήσεων:

    Όλα τα παράλληλα threads θα ξεκινούν και θα δουλεύουν με το ίδιο αρχικό διάνυσμα συντελεστών w

     Σε όλη τη διάρκεια της επεξεργασίας των παρατηρήσεων (ζευγών), δεν θα αλλάζει το w

     Κάθε thread θα δημιουργεί το δικό του ∇J(w,b), το οποίο θα είναι ουσιαστικά ένα διάνυσμα τροποποίησης
     του w.

     Στο τέλος της επεξεργασίας των threads, θα συγχρονίζονται τα threads και θα λαμβάνεται ο μέσος όρος για
     όλα τα ∇J(w,b) που θα έχουν υπολογίσει τα επιμέρους threads.

     Με βάση αυτό το μέσο όρο (και το learning rate η), θα υπολογίζεται η νέα τιμή του w.

     Ξεκινούν να δουλεύουν τα νέα jobs στα threads με το ίδιο νέο διάνυσμα συντελεστών w.

     for (int i = 0; i < batch_sz; i++) {
        int j;
        for (j = 0; j < reg->weights_len; j++) {
            //j is inner loop for cache efficiency
            Deltas[j] += reg->learning_rate * (Ps[i] - Ys[i]) * Xs[i * reg->weights_len + j];
        }
        //Delta for the bias
        Deltas[j] += reg->learning_rate * (Ps[i] - Ys[i]);
    }
     */

    Job job = NULL;
    for (int i = 0; i < batch_sz; i++) {
        job = js_create_job((void *(*)(void *)) loop, JOB_ARG(reg), JOB_ARG(Deltas), JOB_ARG(Xs), JOB_ARG(Ys),
                            JOB_ARG(Ps), JOB_ARG(i), NULL);
        js_submit_job(js, job);
    }

    js_execute_all_jobs(js);
    js_wait_all_jobs(js);

    /* update the weights */
    float max_delta = .0;
    int j;
    for (j = 0; j < reg->weights_len; j++) {
        if (fabsf(Deltas[j]) > max_delta)
            max_delta = fabsf(Deltas[j]);

        reg->weights[j] -= Deltas[j];
    }

    reg->bias -= Deltas[j];

    free(Ps);
    free(Deltas);
    return max_delta;
}
