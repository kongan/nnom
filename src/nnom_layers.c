/*
 * Copyright (c) 2018-2019
 * Jianjia Ma, Wearable Bio-Robotics Group (WBR)
 * majianjia@live.com
 *
 * SPDX-License-Identifier: LGPL-3.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2019-02-05     Jianjia Ma   The first version
 */


#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "nnom.h"
#include "nnom_run.h"
#include "nnom_layers.h"
#include "nnom_activations.h"
#include "nnom_out_shape.h"


size_t shape_size(nnom_shape_t *s)
{
	if(s == NULL)
		return 0;
	return s->h*s->w*s->c;
}

nnom_shape_t shape(size_t h, size_t w, size_t c)
{
	nnom_shape_t s;
	s.h = h;
	s.w = w;
	s.c = c;
	return s;
}
nnom_shape_t kernel(size_t h, size_t w)
{
	return shape(h, w, 1);
}
nnom_shape_t stride(size_t h, size_t w)
{
	return  shape(h, w, 1);
}
nnom_qformat_t qformat(int8_t n, int8_t m)
{
	nnom_qformat_t fmt;
	fmt.n = n;
	fmt.m = m;
	return fmt;
}

// this function has to be used while assign a io for a layer. 
// because the io needs to know who is its owner. 
nnom_layer_io_t * io_init(void * owner_layer, nnom_layer_io_t * io)
{
	io->owner = (nnom_layer_t*)owner_layer;
	return io;
}


// Conv2D 
// multiplier of (output/input channel), 
// shape of kernal, shape of strides, weight struct, bias struct
nnom_layer_t* Conv2D(uint32_t filters, nnom_shape_t k, nnom_shape_t s, nnom_padding_t pad_type,
	nnom_weight_t *w, nnom_bias_t *b)
{
	nnom_conv2d_layer_t *layer;
	nnom_buf_t *comp;
	nnom_layer_io_t *in, *out;
	
	// apply a block memory for all the sub handles. 
	size_t mem_size = sizeof(nnom_conv2d_layer_t) + sizeof(nnom_layer_io_t) *2 + sizeof(nnom_buf_t);
	layer = nnom_mem(mem_size);
	if(layer == NULL) 
		return  NULL;
	
	// distribut the memory to sub handles. 
	in = (void *)((uint32_t)layer + sizeof(nnom_conv2d_layer_t));
	out = (void *)((uint32_t)in + sizeof(nnom_layer_io_t));
	comp = (void *)((uint32_t)out + sizeof(nnom_layer_io_t));
	
	// set type in layer parent
	layer->super.type = NNOM_CONV_2D;
	// set buf state
	in->type = LAYER_BUF_TEMP;
	out->type = LAYER_BUF_TEMP;
	comp->type = LAYER_BUF_TEMP;
	// put in & out on the layer. 
	layer->super.in = io_init(layer, in);
	layer->super.out = io_init(layer, out);
	layer->super.comp = comp;
	// set run method & output shape
	layer->super.run = conv2d_run;
	layer->super.comp_out_shape = conv2d_out_shape;

	// get the private parameters 
	layer->kernel = k;
	layer->stride = s;
	layer->bias = b;
	layer->weights = w;
	layer->output_shift = w->shift;		
	layer->bias_shift = w->shift - b->shift; // bias is quantized to have maximum shift of weights
	layer->filter_mult = filters;				// for convs, this means filter number
	layer->padding_type = pad_type;
	
	// padding
	if(layer->padding_type == PADDING_SAME)
	{
		layer->pad.w = (k.w - 1) / 2;
		layer->pad.h = (k.h - 1) / 2;
		layer->pad.c = (k.c - 1) / 2;
	}

	return (nnom_layer_t*)layer;  
}

nnom_layer_t* DW_Conv2D(uint32_t multiplier, nnom_shape_t k, nnom_shape_t s, nnom_padding_t pad_type,  
	nnom_weight_t *w, nnom_bias_t *b)
{
	nnom_layer_t *layer = Conv2D(multiplier, k, s, pad_type, w, b); // passing multiplier in . 
	if(layer != NULL)
	{
		layer->type = NNOM_DW_CONV_2D;
		layer->run = dw_conv2d_run;
		layer->comp_out_shape = dw_conv2d_out_shape;
	}
	return layer;  
}

// No, there is nothing call "fully_connected()", the name just too long 
nnom_layer_t* Dense(size_t output_unit, nnom_weight_t *w, nnom_bias_t *b)
{
	nnom_dense_layer_t *layer;
	nnom_buf_t *comp;
	nnom_layer_io_t *in,*out;

	// apply a block memory for all the sub handles. 
	size_t mem_size = sizeof(nnom_dense_layer_t) + sizeof(nnom_layer_io_t)*2
		+ sizeof(nnom_buf_t);
	layer = nnom_mem(mem_size);
	if(layer == NULL) 
		return  NULL;
	
	// distribut the memory to sub handles. 
	in = (void *)((uint32_t)layer + sizeof(nnom_dense_layer_t));
	out = (void *)((uint32_t)in + sizeof(nnom_layer_io_t));
	comp = (void *)((uint32_t)out + sizeof(nnom_layer_io_t));
	
	// set type in layer parent
	layer->super.type = NNOM_DENSE;
	// set buf state
	in->type = LAYER_BUF_TEMP;
	out->type = LAYER_BUF_TEMP;
	comp->type = LAYER_BUF_TEMP;
	// put in & out on the layer. 
	layer->super.in = io_init(layer, in);
	layer->super.out = io_init(layer, out);
	layer->super.comp = comp;
	// set run and outshape methods
	layer->super.run = dense_run;
	layer->super.comp_out_shape = dense_out_shape;
	
	// set parameters
	layer->bias = b;
	layer->weights = w;
	layer->output_shift = w->shift;		
	layer->bias_shift = w->shift - b->shift;	// bias is quantized to have maximum shift of weights
	layer->output_unit = output_unit;

	return (nnom_layer_t*)layer;
}



// Simple RNN
// unit = output shape
// type of activation 
nnom_rnn_cell_t* SimpleCell(size_t unit, uint32_t activation, nnom_weight_t *w, nnom_bias_t *b)
{
	nnom_simple_rnn_cell_t *cell;
	cell = nnom_mem(sizeof(nnom_simple_rnn_cell_t));
	if(cell == NULL) 
		return  (nnom_rnn_cell_t*)cell;
	// set parameters
	cell->activation = activation;
	cell->super.unit = unit;
	cell->super.run = cell_simple_rnn_run;
	
	cell->bias = b;
	cell->weights = w;
	//cell->output_shift = w->shift;		
	//cell->bias_shift = w->shift - b->shift;	// bias is quantized to have maximum shift of weights
	
	return (nnom_rnn_cell_t*)cell;
}

// RNN
nnom_layer_t* RNN(nnom_rnn_cell_t* cell, bool return_sequence)
{
	nnom_rnn_layer_t *layer;
	nnom_buf_t *comp;
	nnom_layer_io_t *in,*out;

	// apply a block memory for all the sub handles. 
	size_t mem_size = sizeof(nnom_rnn_layer_t) + sizeof(nnom_layer_io_t)*2
		+ sizeof(nnom_buf_t);
	layer = nnom_mem(mem_size);
	if(layer == NULL) 
		return  NULL;
	
	// distribut the memory to sub handles. 
	in = (void *)((uint32_t)layer + sizeof(nnom_rnn_layer_t));
	out = (void *)((uint32_t)in + sizeof(nnom_layer_io_t));
	comp = (void *)((uint32_t)out + sizeof(nnom_layer_io_t));
	
	// set type in layer parent
	layer->super.type = NNOM_RNN;
	// set buf state
	in->type = LAYER_BUF_TEMP;
	out->type = LAYER_BUF_TEMP;
	comp->type = LAYER_BUF_RESERVED;			// reserve buf for RNN state (statfulness)
	// put in & out on the layer. 
	layer->super.in = io_init(layer, in);
	layer->super.out = io_init(layer, out);
	layer->super.comp = comp;
	// set run and outshape methods
	layer->super.run = rnn_run;
	layer->super.comp_out_shape = rnn_out_shape;
	
	// rnn parameters.
	layer->return_sequence = return_sequence;
	layer->cell = cell;

	return (nnom_layer_t*)layer;
}

nnom_layer_t* Activation(nnom_activation_t * act)
{
	nnom_activation_layer_t* layer;
	nnom_layer_io_t *in, *out;
	
	// apply a block memory for all the sub handles. 
	size_t mem_size = sizeof(nnom_activation_layer_t) + sizeof(nnom_layer_io_t)*2;
	layer = nnom_mem(mem_size);
	if(layer == NULL) 
		return  NULL;
	
	// distribut the memory to sub handles. 
	in = (void *)((uint32_t)layer + sizeof(nnom_activation_layer_t));
	out = (void *)((uint32_t)in + sizeof(nnom_layer_io_t));

	// set type in layer parent
	layer->super.type = NNOM_ACTIVATION;
	layer->super.run = activation_run;
	layer->super.comp_out_shape = default_out_shape;
	// set buf state
	in->type = LAYER_BUF_TEMP;
	out->type = LAYER_BUF_NULL; // when a layer's io is set to NULL, both will point to same mem.
	// put in & out on the layer. 
	layer->super.in = io_init(layer, in);
	layer->super.out = io_init(layer, out);
	
	// set activation to layer
	layer->act = act;

	return (nnom_layer_t*)layer;
}

nnom_layer_t* ReLU(void)
{
	nnom_layer_t *layer = Activation(act_relu());
	if(layer == NULL) 
		return  NULL;
	
	// set type in layer parent
	layer->type = NNOM_RELU;
	return layer;
}

nnom_layer_t* Sigmoid(void)
{
	nnom_layer_t *layer = Activation(act_sigmoid());
	if(layer == NULL) 
		return  NULL;
	
	// set type in layer parent
	layer->type = NNOM_SIGMOID;
	return layer;
}

nnom_layer_t* TanH(void)
{
	nnom_layer_t *layer = Activation(act_tanh());
	if(layer == NULL) 
		return  NULL;
	// set type in layer parent
	layer->type = NNOM_TANH;
	return layer;
}




nnom_layer_t* Softmax(void)
{
	nnom_layer_t *layer;
	nnom_layer_io_t *in,*out;
	
	// apply a block memory for all the sub handles. 
	size_t mem_size = sizeof(nnom_layer_t) + sizeof(nnom_layer_io_t)*2;
	layer = nnom_mem(mem_size);
	if(layer == NULL) 
		return  NULL;
	
	// distribut the memory to sub handles. 
	in = (void *)((uint32_t)layer + sizeof(nnom_layer_t));
	out = (void *)((uint32_t)in + sizeof(nnom_layer_io_t));
	
	// set type in layer parent
	layer->type = NNOM_SOFTMAX;
	layer->run = softmax_run;
	layer->comp_out_shape = default_out_shape;
	// set buf state
	in->type = LAYER_BUF_TEMP;
	out->type = LAYER_BUF_TEMP;
	// put in & out on the layer. 
	layer->in = io_init(layer, in);
	layer->out = io_init(layer, out);

	return layer;
}

nnom_layer_t* MaxPool(nnom_shape_t k, nnom_shape_t s, nnom_padding_t pad_type)
{
	nnom_maxpool_layer_t *layer;
	nnom_buf_t *comp;
	nnom_layer_io_t *in, *out;

	// apply a block memory for all the sub handles. 
	size_t mem_size = sizeof(nnom_maxpool_layer_t) + sizeof(nnom_layer_io_t) * 2 
		+ sizeof(nnom_buf_t);
	layer = nnom_mem(mem_size);
	if(layer == NULL) 
		return  NULL;
	
	// distribut the memory to sub handles. 
	in = (void *)((uint32_t)layer + sizeof(nnom_maxpool_layer_t));
	out = (void *)((uint32_t)in + sizeof(nnom_layer_io_t));
	comp = (void *)((uint32_t)out + sizeof(nnom_layer_io_t));
	
	// set type in layer parent
	layer->super.type = NNOM_MAXPOOL;
	layer->super.run = maxpool_run;
	layer->super.comp_out_shape = maxpooling_out_shape;
	// set buf state
	in->type = LAYER_BUF_TEMP;
	out->type = LAYER_BUF_TEMP;
	comp->type = LAYER_BUF_TEMP;
	// put in & out on the layer. 
	layer->super.in = io_init(layer, in);
	layer->super.out = io_init(layer, out);
	layer->super.comp = comp;

	// set parameters
	layer->kernel = k;
	layer->stride = s;
	layer->padding_type = pad_type;
	
	// padding
	if(layer->padding_type == PADDING_SAME)
	{
		layer->pad.h = (k.h - 1) / 2;
		layer->pad.w = (k.w - 1) / 2;
		layer->pad.c = 1;				// no meaning
	}
	else
	{
		layer->pad.h = 0;
		layer->pad.w = 0;
		layer->pad.c = 0;
	}
	return (nnom_layer_t*)layer;
}

nnom_layer_t* AvgPool(nnom_shape_t k, nnom_shape_t s, nnom_padding_t pad_type)
{
	nnom_layer_t * layer = MaxPool(k, s, pad_type);
	
	if(layer != NULL)
	{
		layer->type = NNOM_AVGPOOL;
		layer->run = avgpool_run;
		layer->comp_out_shape = maxpooling_out_shape; // same as max pool
	}
	return (nnom_layer_t*)layer;
}


nnom_layer_t* Flatten(void)
{
	nnom_layer_t *layer;
	nnom_layer_io_t *in, *out;

	// apply a block memory for all the sub handles. 
	size_t mem_size = sizeof(nnom_layer_t) + sizeof(nnom_layer_io_t)*2;
	layer = nnom_mem(mem_size);
	if(layer == NULL) 
		return  NULL;
	
	// distribut the memory to sub handles. 
	in = (void *)((uint32_t)layer + sizeof(nnom_layer_t));
	out = (void *)((uint32_t)in + sizeof(nnom_layer_io_t));
	
	// set type in layer parent
	layer->type = NNOM_FLATTEN;
	layer->run = flatten_run;
	layer->comp_out_shape = flatten_out_shape;
	// set buf state
	in->type = LAYER_BUF_TEMP;
	out->type = LAYER_BUF_NULL;
	// put in & out on the layer. 
	layer->in = io_init(layer, in);
	layer->out = io_init(layer, out);

	return layer;
}


nnom_layer_t* Input(nnom_shape_t input_shape, nnom_qformat_t fmt, void* p_buf)
{ 
	nnom_io_layer_t *layer;
	nnom_layer_io_t *in, *out;

	// apply a block memory for all the sub handles. 
	size_t mem_size = sizeof(nnom_io_layer_t) + sizeof(nnom_layer_io_t) * 2;
	layer = nnom_mem(mem_size);
	if(layer == NULL) 
		return  NULL;
	
	// distribut the memory to sub handles. 
	in = (void *)((uint32_t)layer + sizeof(nnom_io_layer_t));
	out = (void *)((uint32_t)in + sizeof(nnom_layer_io_t));
	
	// set type in layer parent
	layer->super.type = NNOM_INPUT;
	layer->super.run = input_run;
	layer->super.comp_out_shape = input_out_shape;
	// set buf state
	in->type = LAYER_BUF_TEMP;
	out->type = LAYER_BUF_NULL; 
	// put in & out on the layer. 
	layer->super.in = io_init(layer, in);
	layer->super.in->shape = input_shape; // it is necessary to set input shape in layer wrapper. 
	layer->super.out = io_init(layer, out);

	// set parameters
	layer->shape = input_shape;
	layer->format = fmt;
	layer->buf = p_buf;

	return (nnom_layer_t *)layer;
}

nnom_layer_t* Output(nnom_shape_t output_shape, nnom_qformat_t fmt, void* p_buf)
{ 
	// they are acturally the same.. expect the type defined
	nnom_layer_t *layer = Input(output_shape, fmt, p_buf);
	if(layer != NULL)
	{
		layer->type = NNOM_OUTPUT;
		layer->run = output_run;
		layer->comp_out_shape = output_out_shape;
	}
	return layer;
}

// TODO: extended to multiple IO layer
nnom_layer_t* Lambda(nnom_status_t (*run)(nnom_layer_t*), nnom_status_t (*oshape)(nnom_layer_t*), void * parameters)
{
	nnom_lambda_layer_t *layer;
	nnom_layer_io_t *in, *out;
	
	// apply a block memory for all the sub handles. 
	size_t mem_size = sizeof(nnom_io_layer_t) + sizeof(nnom_layer_io_t) * 2;
	layer = nnom_mem(mem_size);
	if(layer == NULL) 
		return  NULL;
	
	// distribut the memory to sub handles. 
	in = (void *)((uint32_t)layer + sizeof(nnom_lambda_layer_t));
	out = (void *)((uint32_t)in + sizeof(nnom_layer_io_t));
	
	// set buf type. 
	in->type = LAYER_BUF_TEMP;
	out->type = LAYER_BUF_TEMP; 
	
	// set io modules to the layer
	layer->super.in = io_init(layer, in);
	layer->super.out = io_init(layer, out);
	// layer type
	layer->super.type = NNOM_LAMBDA;
	
	// set run method and user parameters. 
	layer->super.run = run;
	layer->parameters = parameters;
	
	// output shape method. pass NULL in will use the default outshape method, which set the output shape same as input shape. 
	if(oshape == NULL)
		layer->super.comp_out_shape = default_out_shape;
	else 
		layer->super.comp_out_shape = oshape;
	return (nnom_layer_t *)layer;
}

// concate method
nnom_layer_t* Concat(int8_t axis)
{
	nnom_concat_layer_t *layer;
	nnom_layer_io_t *in1, *in2, *out;
	size_t mem_size;	

	// apply a block memory for all the sub handles. 
	mem_size = sizeof(nnom_concat_layer_t) + sizeof(nnom_layer_io_t) * 3;
	layer = nnom_mem(mem_size);
	if(layer == NULL) 
		return  NULL;
	
	// distribut the memory to sub handles. 
	in1 = (void *)((uint32_t)layer + sizeof(nnom_concat_layer_t));
	in2 = (void *)((uint32_t)in1 + sizeof(nnom_layer_io_t));
	out = (void *)((uint32_t)in2 + sizeof(nnom_layer_io_t));
	
	// set type in layer parent
	layer->super.type = NNOM_CONCAT;
	layer->super.run = concat_run;
	layer->super.comp_out_shape = concatenate_out_shape;
	// set buf state
	in1->type = LAYER_BUF_TEMP;
	in2->type = LAYER_BUF_TEMP;
	out->type = LAYER_BUF_TEMP; 
	// put in & out on the layer. 
	layer->super.in = io_init(layer, in1);
	layer->super.in->aux = io_init(layer, in2);		// set the second input to the aux input. 
	layer->super.out = io_init(layer, out);
	
	// parameters 
	layer->axis = axis;

	return (nnom_layer_t*)layer;
}

static nnom_layer_t *_same_shape_2in_1out_layer()
{
	nnom_layer_t *layer;
	nnom_layer_io_t *in1, *in2, *out;
	size_t mem_size;	

	// apply a block memory for all the sub handles. 
	mem_size = sizeof(nnom_layer_t) + sizeof(nnom_layer_io_t) * 3;
	layer = nnom_mem(mem_size);
	if(layer == NULL) 
		return  NULL;
	
	// distribut the memory to sub handles. 
	in1 = (void *)((uint32_t)layer + sizeof(nnom_layer_t));
	in2 = (void *)((uint32_t)in1 + sizeof(nnom_layer_io_t));
	out = (void *)((uint32_t)in2 + sizeof(nnom_layer_io_t));
	
	// set type in layer parent
	layer->comp_out_shape = same_shape_2in_1out_out_shape;
	// set buf state
	in1->type = LAYER_BUF_TEMP;
	in2->type = LAYER_BUF_TEMP;
	out->type = LAYER_BUF_TEMP; 
	// put in & out on the layer. 
	layer->in = io_init(layer, in1);
	layer->in->aux = io_init(layer, in2);		// set the second input to the aux input. 
	layer->out = io_init(layer, out);

	return layer;
}

nnom_layer_t * Add()
{
	nnom_layer_t *layer = _same_shape_2in_1out_layer();
	if(layer == NULL) 
		return  NULL;
	// set type in layer parent
	layer->type = NNOM_ADD;
	layer->run = add_run;
	return layer;
}
nnom_layer_t * Sub()
{
	nnom_layer_t *layer = _same_shape_2in_1out_layer();
	if(layer == NULL) 
		return  NULL;
	// set type in layer parent
	layer->type = NNOM_SUB;
	layer->run = sub_run;
	return layer;
}
nnom_layer_t * Mult()
{
	nnom_layer_t *layer = _same_shape_2in_1out_layer();
	if(layer == NULL) 
		return  NULL;
	// set type in layer parent
	layer->type = NNOM_MULT;
	layer->run = mult_run;
	return layer;
}















