
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>

#undef DEBUG

#include "config.h"
#include "rtk.h"
#include "gui.h"

// models that have fewer rectangles than this get matrix rendered when dragged
#define STG_LINE_THRESHOLD 40

#define LASER_FILLED 1

#define BOUNDINGBOX 0

// single static application visible to all funcs in this file
static rtk_app_t *app = NULL; 

// here's a figure anyone can draw in for debug purposes
rtk_fig_t* fig_debug = NULL;

void gui_startup( int* argc, char** argv[] )
{
  PRINT_DEBUG( "gui startup" );

  rtk_init(argc, argv);
  
  app = rtk_app_create();
  rtk_app_main_init( app );
}

void gui_poll( void )
{
  //PRINT_DEBUG( "gui poll" );
  rtk_app_main_loop( app );
}

void gui_shutdown( void )
{
  PRINT_DEBUG( "gui shutdown" );
  rtk_app_main_term( app );  
}

rtk_fig_t* gui_grid_create( rtk_canvas_t* canvas, rtk_fig_t* parent, 
			    double origin_x, double origin_y, double origin_a, 
			    double width, double height, 
			    double major, double minor )
{
  rtk_fig_t* grid = NULL;
  
  grid = rtk_fig_create( canvas, parent, STG_LAYER_GRID );
  
  if( minor > 0)
    {
      rtk_fig_color_rgb32( grid, stg_lookup_color(STG_GRID_MINOR_COLOR ) );
      rtk_fig_grid( grid, origin_x, origin_y, width, height, minor);
    }
  if( major > 0)
    {
      rtk_fig_color_rgb32( grid, stg_lookup_color(STG_GRID_MAJOR_COLOR ) );
      rtk_fig_grid( grid, origin_x, origin_y, width, height, major);
    }

  return grid;
}


gui_window_t* gui_window_create( world_t* world, int xdim, int ydim )
{
  gui_window_t* win = calloc( sizeof(gui_window_t), 1 );

  win->canvas = rtk_canvas_create( app );
  
  win->world = world;
  
  // enable all objects on the canvas to find our window object
  win->canvas->userdata = (void*)win; 

  GtkHBox* hbox = GTK_HBOX(gtk_hbox_new( TRUE, 10 ));
  
  win->statusbar = GTK_STATUSBAR(gtk_statusbar_new());
  gtk_statusbar_set_has_resize_grip( win->statusbar, FALSE );
  gtk_box_pack_start(GTK_BOX(hbox), 
		     GTK_WIDGET(win->statusbar), FALSE, TRUE, 0);
  
  win->timelabel = GTK_LABEL(gtk_label_new("0:00:00"));
  gtk_box_pack_end(GTK_BOX(hbox), 
		   GTK_WIDGET(win->timelabel), FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(win->canvas->layout), 
		     GTK_WIDGET(hbox), FALSE, TRUE, 0);


  char txt[256];
  snprintf( txt, 256, "Stage v%s", VERSION );
  gtk_statusbar_push( win->statusbar, 0, txt ); 

  rtk_canvas_size( win->canvas, xdim, ydim );
  
  GString* titlestr = g_string_new( "Stage: " );
  g_string_append_printf( titlestr, "%s", world->token );
  
  rtk_canvas_title( win->canvas, titlestr->str );
  g_string_free( titlestr, TRUE );
  
  // todo - destructors for figures
  //win->guimods = g_hash_table_new( g_int_hash, g_int_equal );
  
  win->bg = rtk_fig_create( win->canvas, NULL, 0 );
  
  double width = 10;//world->size.x;
  double height = 10;//world->size.y;

  // draw the axis origin lines
  //rtk_fig_color_rgb32( win->grid, stg_lookup_color(STG_GRID_AXIS_COLOR) );  
  //rtk_fig_line( win->grid, 0, 0, width, 0 );
  //rtk_fig_line( win->grid, 0, 0, 0, height );

  win->show_grid = TRUE;
  win->show_matrix = FALSE;
  win->show_geom = FALSE;
  win->show_rects = TRUE;

  win->movie_exporting = FALSE;
  win->movie_count = 0;
  win->movie_speed = STG_DEFAULT_MOVIE_SPEED;
  
  win->poses = rtk_fig_create( win->canvas, NULL, 0 );

  // start in the center, fully zoomed out
  rtk_canvas_scale( win->canvas, 1.1*width/xdim, 1.1*width/xdim );
  rtk_canvas_origin( win->canvas, width/2.0, height/2.0 );

  gui_window_menus_create( win );

  return win;
}

void gui_window_destroy( gui_window_t* win )
{
  PRINT_DEBUG( "gui window destroy" );

  //g_hash_table_destroy( win->guimods );

  rtk_canvas_destroy( win->canvas );
  rtk_fig_destroy( win->bg );
  rtk_fig_destroy( win->poses );
}

gui_window_t* gui_world_create( world_t* world )
{
  PRINT_DEBUG( "gui world create" );
  
  gui_window_t* win = gui_window_create( world, 
					 STG_DEFAULT_WINDOW_WIDTH,
					 STG_DEFAULT_WINDOW_HEIGHT );
  
  // show the window
  gtk_widget_show_all(win->canvas->frame);

  return win;
}


// render every occupied matrix pixel as an unfilled rectangle
typedef struct
{
  //stg_matrix_t* matrix;
  double ppm;
  rtk_fig_t* fig;
} gui_mf_t;


void render_matrix_cell( gui_mf_t*mf, stg_matrix_coord_t* coord )
{
  double pixel_size = 1.0 / mf->ppm;  
  rtk_fig_rectangle( mf->fig, 
		     coord->x * pixel_size + pixel_size/2.0, 
		     coord->y * pixel_size + pixel_size/2.0, 0, 
		     //coord->x + pixel_size/2.0, 
		     //coord->y + pixel_size/2.0, 0, 
		     pixel_size, pixel_size, 0 );
}

void render_matrix_cell_cb( gpointer key, gpointer value, gpointer user )
{
  GPtrArray* arr = (GPtrArray*)value;

  if( arr->len > 0 )
    {  
      stg_matrix_coord_t* coord = (stg_matrix_coord_t*)key;
      gui_mf_t* mf = (gui_mf_t*)user;
      
      render_matrix_cell( mf, coord );
    }
}


// useful debug function allows plotting the matrix
void gui_world_matrix( world_t* world, gui_window_t* win )
{
  if( win->matrix == NULL )
    {
      win->matrix = rtk_fig_create(win->canvas,win->bg,STG_LAYER_MATRIX);
      rtk_fig_color_rgb32(win->matrix, stg_lookup_color(STG_MATRIX_COLOR));
      
    }
  else
    rtk_fig_clear( win->matrix );
  
  gui_mf_t mf;
  mf.fig = win->matrix;

  mf.ppm = world->matrix->ppm;
  g_hash_table_foreach( world->matrix->table, render_matrix_cell_cb, &mf );
  
  mf.ppm = world->matrix->medppm;
  g_hash_table_foreach( world->matrix->table, render_matrix_cell_cb, &mf );
  
  mf.ppm = world->matrix->bigppm;
  g_hash_table_foreach( world->matrix->bigtable, render_matrix_cell_cb, &mf );
}

void gui_pose( rtk_fig_t* fig, model_t* mod )
{
  rtk_fig_arrow_ex( fig, 0,0, mod->pose.x, mod->pose.y, 0.05 );
}


void gui_pose_cb( gpointer key, gpointer value, gpointer user )
{
  gui_pose( (rtk_fig_t*)user, (model_t*)value );
}


void gui_world_update( world_t* world )
{
  //PRINT_DEBUG( "gui world update" );
  
  gui_window_t* win = world->win;
  
  if( win->show_matrix ) gui_world_matrix( world, win );
  
  char clock[256];
  snprintf( clock, 255, "%lu:%2lu:%2lu.%3lu\n",
	    world->sim_time / 3600000, // hours
	    (world->sim_time % 3600000) / 60000, // minutes
	    (world->sim_time % 60000) / 1000, // seconds
	    world->sim_time % 1000 ); // milliseconds
  
  //puts( clock );
  gtk_label_set_text( win->timelabel, clock );
   
  rtk_canvas_render( win->canvas );
}

void gui_world_destroy( world_t* world )
{
  PRINT_DEBUG( "gui world destroy" );
    
  if( world->win && world->win->canvas ) 
    rtk_canvas_destroy( world->win->canvas );
  else
    PRINT_WARN1( "can't find a window for world %d", world->id );
}


const char* gui_model_describe(  model_t* mod )
{
  static char txt[256];
  
  snprintf(txt, sizeof(txt), "\"%s\" (%d:%d) pose: [%.2f,%.2f,%.2f]",  
	   mod->token, mod->world->id, mod->id,  
	   mod->pose.x, mod->pose.y, mod->pose.a  );
  
  return txt;
}


// Process mouse events 
void gui_model_mouse(rtk_fig_t *fig, int event, int mode)
{
  //PRINT_DEBUG2( "ON MOUSE CALLED BACK for %p with userdata %p", fig, fig->userdata );
    // each fig links back to the Entity that owns it
  model_t* mod = (model_t*)fig->userdata;
  assert( mod );

  gui_window_t* win = mod->world->win;
  assert(win);

  guint cid; // statusbar context id
  char txt[256];
    
  static stg_velocity_t capture_vel;
  stg_pose_t pose;  
  stg_velocity_t zero_vel;
  memset( &zero_vel, 0, sizeof(zero_vel) );

  switch (event)
    {
    case RTK_EVENT_PRESS:
      // store the velocity at which we grabbed the model
      memcpy( &capture_vel, &mod->velocity, sizeof(capture_vel) );
      model_set_prop( mod, STG_PROP_VELOCITY, &zero_vel, sizeof(zero_vel) );

      // DELIBERATE NO-BREAK      

    case RTK_EVENT_MOTION:       
      // move the object to follow the mouse
      rtk_fig_get_origin(fig, &pose.x, &pose.y, &pose.a );
      
      // TODO - if there are more motion events pending, do nothing.
      //if( !gtk_events_pending() )
	
      // only update simple objects on drag
      //if( mod->lines->len < STG_LINE_THRESHOLD )
      model_set_prop( mod, STG_PROP_POSE, &pose, sizeof(pose) );
      
      // display the pose
      snprintf(txt, sizeof(txt), "Dragging: %s", gui_model_describe(mod)); 
      cid = gtk_statusbar_get_context_id( win->statusbar, "on_mouse" );
      gtk_statusbar_pop( win->statusbar, cid ); 
      gtk_statusbar_push( win->statusbar, cid, txt ); 
      //printf( "STATUSBAR: %s\n", txt );
      break;
      
    case RTK_EVENT_RELEASE:
      // move the entity to its final position
      rtk_fig_get_origin(fig, &pose.x, &pose.y, &pose.a );
      model_set_prop( mod, STG_PROP_POSE, &pose, sizeof(pose) );
      
      // and restore the velocity at which we grabbed it
      model_set_prop( mod, STG_PROP_VELOCITY, &capture_vel, sizeof(capture_vel) );

      // take the pose message from the status bar
      cid = gtk_statusbar_get_context_id( win->statusbar, "on_mouse" );
      gtk_statusbar_pop( win->statusbar, cid ); 
      break;      
      
    default:
      break;
    }

  return;
}

void gui_model_parent( model_t* model )
{
  
}

void gui_model_grid( model_t* model )
{  
  gui_window_t* win = model->world->win;
  gui_model_t* gmod = gui_model_figs(model);

  assert( gmod );

  if( gmod->grid )
    rtk_fig_destroy( gmod->grid );
  
  if( win->show_grid && model->grid )
    gmod->grid = gui_grid_create( win->canvas, gmod->top, 
				  0, 0, 0, 
				  model->geom.size.x, model->geom.size.y, 1.0, 0 );
}
  
void gui_model_create( model_t* model )
{
  PRINT_DEBUG( "gui model create" );
  
  gui_window_t* win = model->world->win;  
  rtk_fig_t* parent_fig = win->bg;
  
  // attach to our parent's fig if there is one
  if( model->parent )
    parent_fig = model->parent->gui.top;
  

  //gui_model_t* gmod = calloc( sizeof(gui_model_t),1 );
  //g_hash_table_replace( win->guimods, &model->id, gmod );
  
  gui_model_t* gmod = &model->gui;
  memset( gmod, 0, sizeof(gui_model_t) );
  
  gmod->grid = NULL;
  
  gmod->top = 
    rtk_fig_create( model->world->win->canvas, parent_fig, STG_LAYER_BODY );

  gmod->geom = 
    rtk_fig_create( model->world->win->canvas, parent_fig, STG_LAYER_GEOM );

  //gmod->top = 
  //rtk_fig_create( model->world->win->canvas, parent_fig, STG_LAYER_BODY );
  
  gmod->top->userdata = model;

  rtk_fig_movemask( gmod->top, model->movemask );
  rtk_fig_add_mouse_handler( gmod->top, gui_model_mouse );
  
  gui_model_render( model );
}

gui_model_t* gui_model_figs( model_t* model )
{
  return &model->gui;
  //gui_window_t* win = model->world->win;
  //return (gui_model_t*)g_hash_table_lookup( win->guimods, &model->id );
}

// draw a model from scratch
void gui_model_render( model_t* model )
{
  PRINT_DEBUG( "gui model render" );
  
  rtk_fig_t* fig = gui_model_figs(model)->top;
  rtk_fig_clear( fig );
  
  rtk_fig_origin( fig, model->pose.x, model->pose.y, model->pose.a );

#if BOUNDINGBOX 
  rtk_fig_color_rgb32( fig, stg_lookup_color(STG_BOUNDINGBOX_COLOR) );
  rtk_fig_rectangle( fig, 0,0,0, model->size.x, model->size.y, 0 );
#endif
  
  gui_model_lines( model );
  gui_model_nose( model );
  gui_model_geom( model );
  gui_model_grid( model );
}

void gui_model_destroy( model_t* model )
{
  PRINT_DEBUG( "gui model destroy" );

  // TODO - It's too late to kill the figs - the canvas is gone! fix this?

  if( model->gui.top ) rtk_fig_destroy( model->gui.top );
  if( model->gui.grid ) rtk_fig_destroy( model->gui.grid );
  if( model->gui.geom ) rtk_fig_destroy( model->gui.geom );
  // todo - erase the property figs
}

void gui_model_pose( model_t* mod )
{
  //PRINT_DEBUG( "gui model pose" );
  rtk_fig_origin( gui_model_figs(mod)->top, 
		  mod->pose.x, mod->pose.y, mod->pose.a );
}


void gui_model_lines( model_t* mod )
{
  rtk_fig_t* fig = gui_model_figs(mod)->top;
  
  rtk_fig_clear( fig );

  rtk_fig_color_rgb32( fig, mod->color );

  PRINT_DEBUG1( "rendering %d lines", mod->lines->len );

  double localx = mod->local_pose.x;
  double localy = mod->local_pose.y;
  double locala = mod->local_pose.a;
  
  double cosla = cos(locala);
  double sinla = sin(locala);
  
  // draw lines too
  int l;
  for( l=0; l<mod->lines->len; l++ )
    {
      stg_line_t* line = &g_array_index( mod->lines, stg_line_t, l );
      
      double x1 = localx + line->x1 * cosla - line->y1 * sinla;
      double y1 = localy + line->x1 * sinla + line->y1 * cosla;
      double x2 = localx + line->x2 * cosla - line->y2 * sinla;
      double y2 = localy + line->x2 * sinla + line->y2 * cosla;
      
      rtk_fig_line( fig, x1,y1, x2,y2 );
    }
}



void gui_model_geom( model_t* mod )
{
  rtk_fig_t* fig = gui_model_figs(mod)->geom;
  gui_window_t* win = mod->world->win;

  rtk_fig_clear( fig );

  if( win->show_geom )
    {
      rtk_fig_color_rgb32( fig, 0 );
      
      double localx = mod->local_pose.x;
      double localy = mod->local_pose.y;
      double locala = mod->local_pose.a;
      
      if( mod->boundary )
	rtk_fig_rectangle( fig, localx, localy, locala, 
			   mod->geom.size.x, mod->geom.size.y, 0 ); 
      
      // draw the origin and the offset arrow
      double orgx = 0.05;
      double orgy = 0.03;
      rtk_fig_arrow_ex( fig, -orgx, 0, orgx, 0, 0.02 );
      rtk_fig_line( fig, 0,-orgy, 0, orgy );
      rtk_fig_line( fig, 0, 0, localx, localy );
      //rtk_fig_line( fig, localx-orgx, localy, localx+orgx, localy );
      rtk_fig_arrow( fig, localx, localy, locala, orgx, 0.02 );  
      rtk_fig_arrow( fig, localx, localy, locala-M_PI/2.0, orgy, 0.0 );
      rtk_fig_arrow( fig, localx, localy, locala+M_PI/2.0, orgy, 0.0 );
      rtk_fig_arrow( fig, localx, localy, locala+M_PI, orgy, 0.0 );
      //rtk_fig_arrow( fig, localx, localy, 0.0, orgx, 0.0 );  
    }
}

// add a nose  indicating heading  
void gui_model_nose( model_t* mod )
{
  if( mod->nose )
    { 
      rtk_fig_t* fig = gui_model_figs(mod)->top;      
      rtk_fig_color_rgb32( fig, mod->color );
      
      // draw a line from the center to the front of the model
      rtk_fig_line( fig, 
		    mod->local_pose.x, 
		    mod->local_pose.y, 
		    mod->geom.size.x/2, 0 );
    }
}

void gui_model_movemask( model_t* mod )
{
  // we can only manipulate top-level figures
  //if( ent->parent == NULL )
  
  // figure gets the same movemask as the model, ONLY if it is a
  // top-level object
  if( mod->parent == NULL )
    rtk_fig_movemask( gui_model_figs(mod)->top, mod->movemask);  
      

  if( mod->movemask )
    // Set the mouse handler
    rtk_fig_add_mouse_handler( gui_model_figs(mod)->top, gui_model_mouse );
}


void gui_model_update( model_t* mod, stg_prop_type_t prop )
{
  //PRINT_DEBUG3( "gui update for %d:%s prop %s", 
  //	ent->id, ent->name->str, stg_property_string(prop) );
  
  assert( mod );
  //assert( prop >= 0 );
  //assert( prop < STG_PROP_PROP_COUNT );
  
  switch( prop )
    {
    case 0: // for these we basically redraw everything
    case STG_PROP_BOUNDARY:      
    case STG_PROP_GEOM:
    case STG_PROP_COLOR:
    case STG_PROP_NOSE:
    case STG_PROP_MOVEMASK:
    case STG_PROP_GRID:
      gui_model_render( mod );
      gui_model_movemask( mod );
      break;
      
    case STG_PROP_POSE:
      gui_model_pose( mod );
      break;

    case STG_PROP_LINES:
      gui_model_lines( mod );
      break;

    case STG_PROP_PARENT: 
      gui_model_parent( mod );
      break;
      
      // do nothing for these things
    case STG_PROP_LASERRETURN:
    case STG_PROP_SONARRETURN:
    case STG_PROP_OBSTACLERETURN:
    case STG_PROP_VISIONRETURN:
    case STG_PROP_PUCKRETURN:
    case STG_PROP_VELOCITY:
    case STG_PROP_LOSMSG:
    case STG_PROP_LOSMSGCONSUME:
    case STG_PROP_NAME:
    case STG_PROP_INTERVAL:
    case STG_PROP_MATRIXRENDER:
    case STG_PROP_BLOBCONFIG:
    case STG_PROP_RANGERCONFIG:
    case STG_PROP_LASERCONFIG:
    case STG_PROP_FIDUCIALCONFIG:
      break;

    default:
      PRINT_WARN2( "property change %d(%s) unhandled by gui", 
		   prop, stg_property_string(prop) ); 
      break;
    } 
}
 