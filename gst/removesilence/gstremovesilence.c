/* GStreamer
 * Copyright (C) 2011 Tiago Katcipis <tiagokatcipis@gmail.com>
 * Copyright (C) 2011 Paulo Pizarro  <paulo.pizarro@gmail.com>
 * Copyright (C) 2012-2016 Nicola Murino  <nicola.murino@gmail.com>
 * Copyright (C) 2016 Sagar Pilkhwal  <pilkhwal.sagar@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-removesilence
 *
 * Removes all silence periods from an audio stream, dropping silence buffers.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 -v -m filesrc location="audiofile" ! decodebin ! removesilence remove=true ! wavenc ! filesink location=without_audio.wav
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/audio/audio.h>
#include <glib-2.0/gobject/gmarshal.h>
#include <glib-object.h>
#include <string.h>

#include "gstremovesilence.h"


GST_DEBUG_CATEGORY_STATIC (gst_remove_silence_debug);
#define GST_CAT_DEFAULT gst_remove_silence_debug
#define DEFAULT_VAD_HYSTERESIS  960     /* 60 mseg = 480 */
#define MINIMUM_SILENCE_BUFFERS_MIN  0
#define MINIMUM_SILENCE_BUFFERS_MAX  10000
#define MINIMUM_SILENCE_BUFFERS_DEF  0
#define MINIMUM_SILENCE_TIME_MIN  0
#define MINIMUM_SILENCE_TIME_MAX  10000000000
#define MINIMUM_SILENCE_TIME_DEF  0
#define MAXIMUM_VOICE_BUFFER_SIZE_1 5000
#define MAXIMUM_VOICE_BUFFER_SIZE_2 30000
#define MAXIMUM_VOICE_BUFFER_SIZE_MIN 1000 
#define MAXIMUM_VOICE_BUFFER_SIZE_MAX 100000
#define MAXIMUM_VOICE_BUFFER_SIZE_DEF 20000

/* Filter signals and args */
enum
{
  /* FILL ME */
  SIGNAL_BUFFER,
  LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_REMOVE,
  PROP_HYSTERESIS,
  PROP_SQUASH,
  PROP_SILENT,
  PROP_MINIMUM_SILENCE_BUFFERS,
  PROP_MINIMUM_SILENCE_TIME,
  PROP_MAXIMUM_VOICE_BUFFER_SIZE
};


static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) " GST_AUDIO_NE (S16) ", "
        "layout = (string) interleaved, "
        "rate = (int) 8000, " "channels = (int) 1")); /* Hardcoded */ 

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) " GST_AUDIO_NE (S16) ", "
        "layout = (string) interleaved, "
        "rate = (int) [ 1, MAX ], " "channels = (int) 1"));


#define DEBUG_INIT(bla) \
  GST_DEBUG_CATEGORY_INIT (gst_remove_silence_debug, "removesilence", 0, "removesilence element")

#define gst_remove_silence_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstRemoveSilence, gst_remove_silence,
    GST_TYPE_BASE_TRANSFORM, DEBUG_INIT (0));

static void gst_remove_silence_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_remove_silence_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstFlowReturn gst_remove_silence_transform_ip (GstBaseTransform * base,
    GstBuffer * buf);
static void gst_remove_silence_finalize (GObject * obj);

/* GObject vmethod implementations */


//static GstElementClass *parent_class;
static guint gst_remove_silence_signals[LAST_SIGNAL] = { 0 };

/* initialize the removesilence's class */
static void
gst_remove_silence_class_init (GstRemoveSilenceClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->finalize = gst_remove_silence_finalize;
  gobject_class->set_property = gst_remove_silence_set_property;
  gobject_class->get_property = gst_remove_silence_get_property;

  g_object_class_install_property (gobject_class, PROP_REMOVE,
      g_param_spec_boolean ("remove", "Remove",
          "Set to true to remove silence from the stream, false otherwhise",
          FALSE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_HYSTERESIS,
      g_param_spec_uint64 ("hysteresis",
          "Hysteresis",
          "Set the hysteresis (on samples) used on the internal VAD",
          1, G_MAXUINT64, DEFAULT_VAD_HYSTERESIS, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_SQUASH,
      g_param_spec_boolean ("squash", "Squash",
          "Set to true to retimestamp buffers when silence is removed and so avoid timestamp gap",
          FALSE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_SILENT,
      g_param_spec_boolean ("silent", "Silent",
          "Disable/enable bus message notifications for silent detected/finished",
          TRUE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_MINIMUM_SILENCE_BUFFERS,
      g_param_spec_uint ("minimum-silence-buffers", "Minimum silence buffers",
          "Define the minimum number of consecutive silence buffers before "
          "removing silence, 0 means disabled",
          MINIMUM_SILENCE_BUFFERS_MIN, MINIMUM_SILENCE_BUFFERS_MAX,
          MINIMUM_SILENCE_BUFFERS_DEF, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_MINIMUM_SILENCE_TIME,
      g_param_spec_uint64 ("minimum_silence_time",
          "Minimum silence time",
          "Define the minimum silence time in nanoseconds before removing "
          " silence, 0 means disabled",
          MINIMUM_SILENCE_TIME_MIN, MINIMUM_SILENCE_TIME_MAX,
          MINIMUM_SILENCE_TIME_DEF, G_PARAM_READWRITE));
	
  g_object_class_install_property (gobject_class, PROP_MAXIMUM_VOICE_BUFFER_SIZE,
      g_param_spec_uint64 ("max_voice_buffer_size",
          "Maximum voice buffer size",
          "Define the maximum voice buffer size",
          MAXIMUM_VOICE_BUFFER_SIZE_MIN, MAXIMUM_VOICE_BUFFER_SIZE_MAX, MAXIMUM_VOICE_BUFFER_SIZE_DEF, G_PARAM_READWRITE));
	
	
	
  /* Add signals */
	gst_remove_silence_signals[SIGNAL_BUFFER] =
    g_signal_new (	"buffer", 
				  	G_TYPE_FROM_CLASS (klass),
					0, //G_SIGNAL_RUN_LAST,
					0, //G_STRUCT_OFFSET (GstRemoveSilenceClass, buffer), 
					NULL, 
					NULL,
					g_cclosure_marshal_generic,
					G_TYPE_NONE, 
					2,//1, 
					G_TYPE_POINTER, G_TYPE_INT //GST_TYPE_BUFFER
				 );
	
  /* send signals of every voice buffer*/

  gst_element_class_set_static_metadata (gstelement_class,
      "RemoveSilence",
      "Filter/Effect/Audio",
      "Removes all the silence periods from the audio stream.",
      "Tiago Katcipis <tiagokatcipis@gmail.com>\n \
       Paulo Pizarro  <paulo.pizarro@gmail.com>\n \
       Nicola Murino  <nicola.murino@gmail.com>\n \
	   Sagar Pilkhwal <pilkhwal.sagar@gmail.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_template));

  GST_BASE_TRANSFORM_CLASS (klass)->transform_ip =
      GST_DEBUG_FUNCPTR (gst_remove_silence_transform_ip);
}

/* initialize the new element
 * instantiate pads and add them to element
 * set pad calback functions
 * initialize instance structure
 */
static void
gst_remove_silence_init (GstRemoveSilence * filter)
{
  filter->vad = vad_new (DEFAULT_VAD_HYSTERESIS);
  filter->remove = FALSE;
  filter->squash = FALSE;
  filter->ts_offset = 0;
  filter->silence_detected = FALSE;
  filter->silent = FALSE; //default messages are ON
  filter->consecutive_silence_buffers = 0;
  filter->minimum_silence_buffers = MINIMUM_SILENCE_BUFFERS_DEF;
  filter->minimum_silence_time = MINIMUM_SILENCE_TIME_DEF;
  filter->consecutive_silence_time = 0;
  filter->max_voice_buffer_size = MAXIMUM_VOICE_BUFFER_SIZE_DEF;

  if (!filter->vad) {
    GST_DEBUG ("Error initializing VAD !!");
    return;
  }
}

static void
gst_remove_silence_finalize (GObject * obj)
{
  GstRemoveSilence *filter = GST_REMOVE_SILENCE (obj);
  //GST_DEBUG ("Destroying VAD");
  vad_destroy (filter->vad);
  filter->vad = NULL;
  GST_DEBUG ("VAD Destroyed");
  G_OBJECT_CLASS (parent_class)->finalize (obj);
}

static void
gst_remove_silence_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRemoveSilence *filter = GST_REMOVE_SILENCE (object);

  switch (prop_id) {
    case PROP_REMOVE:
      filter->remove = g_value_get_boolean (value);
      break;
    case PROP_HYSTERESIS:
      vad_set_hysteresis (filter->vad, g_value_get_uint64 (value));
      break;
    case PROP_SQUASH:
      filter->squash = g_value_get_boolean (value);
      break;
    case PROP_SILENT:
      filter->silent = g_value_get_boolean (value);
      break;
    case PROP_MINIMUM_SILENCE_BUFFERS:
      filter->minimum_silence_buffers = g_value_get_uint (value);
      break;
    case PROP_MINIMUM_SILENCE_TIME:
      filter->minimum_silence_time = g_value_get_uint64 (value);
      break;
	case PROP_MAXIMUM_VOICE_BUFFER_SIZE:
	  filter->max_voice_buffer_size = g_value_get_uint64 (value);
		  GST_DEBUG(" +++++++++++++ setting max voice: %d", (int) filter->max_voice_buffer_size);
	  break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_remove_silence_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstRemoveSilence *filter = GST_REMOVE_SILENCE (object);

  switch (prop_id) {
    case PROP_REMOVE:
      g_value_set_boolean (value, filter->remove);
      break;
    case PROP_HYSTERESIS:
      g_value_set_uint64 (value, vad_get_hysteresis (filter->vad));
      break;
    case PROP_SQUASH:
      g_value_set_boolean (value, filter->squash);
      break;
    case PROP_SILENT:
      g_value_set_boolean (value, filter->silent);
      break;
    case PROP_MINIMUM_SILENCE_BUFFERS:
      g_value_set_uint (value, filter->minimum_silence_buffers);
      break;
    case PROP_MINIMUM_SILENCE_TIME:
      g_value_set_uint64 (value, filter->minimum_silence_time);
      break;
    case PROP_MAXIMUM_VOICE_BUFFER_SIZE:
      g_value_set_uint64 (value, filter->max_voice_buffer_size);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

void SendMaxVoiceMessage(GstRemoveSilence *filter){
	GstStructure *s;
	GstMessage *m;      

	gint64 buffer_emited = total_buf_emited;
	total_buf_emited = 0; //this needs to be done fast before the next frames comes in as silent

	s = gst_structure_new ("removesilence", "max_voice_reached",G_TYPE_INT64, buffer_emited,  NULL);

	m = gst_message_new_element (GST_OBJECT (filter), s);
	gst_element_post_message (GST_ELEMENT (filter), m);
}

static GstFlowReturn
gst_remove_silence_transform_ip (GstBaseTransform * trans, GstBuffer * inbuf)
{
  GstRemoveSilence *filter = NULL;
  int frame_type;
  GstMapInfo map;
  gboolean consecutive_silence_reached;
  
  filter = GST_REMOVE_SILENCE (trans);

  gst_buffer_map (inbuf, &map, GST_MAP_READ);
       	
  frame_type = vad_update (filter->vad, (gint16 *) map.data, map.size / sizeof (gint16) );
	
  gst_buffer_unmap (inbuf, &map); 
  
	//VAD SILENCE
  if (frame_type == VAD_SILENCE) {
	if (total_buf_emited >= filter->max_voice_buffer_size) {     
		SendMaxVoiceMessage(filter);
		GST_ERROR ("MAX_Voice 1 Max");
    }
	GST_ERROR ("\nSilence detected");
    filter->consecutive_silence_buffers++;
    if (GST_BUFFER_DURATION_IS_VALID (inbuf)) {
      filter->consecutive_silence_time += inbuf->duration;
    } else {
      GST_DEBUG ("Invalid buffer duration, consecutive_silence_time update not possible");
    }
    
    if (filter->minimum_silence_buffers == 0
        && filter->minimum_silence_time == 0) {
      consecutive_silence_reached = TRUE;
    } else {
      consecutive_silence_reached =
          (filter->minimum_silence_buffers > 0
          && filter->consecutive_silence_buffers >=
          filter->minimum_silence_buffers)
          || (filter->minimum_silence_time > 0
          && filter->consecutive_silence_time >= filter->minimum_silence_time);
    }
    
    if (!filter->silence_detected && consecutive_silence_reached) {
      if (!filter->silent) {
        if (GST_BUFFER_PTS_IS_VALID (inbuf)) {
          GstStructure *s;
          GstMessage *m;
          
		  gint64 buffer_emited = total_buf_emited;
			
          total_buf_emited = 0;
          
          s = gst_structure_new ("removesilence", "silence_detected",
              G_TYPE_INT64, buffer_emited /*GST_BUFFER_PTS (inbuf) - filter->ts_offset*/, NULL);
          m = gst_message_new_element (GST_OBJECT (filter), s);
          gst_element_post_message (GST_ELEMENT (filter), m);
          GST_DEBUG (" ---------------------- silence_detected: %d", (int)buffer_emited);
        }
      }
      filter->silence_detected = TRUE;
    }

    if (filter->remove && consecutive_silence_reached) {
      //GST_DEBUG ("Removing silence");
      if (filter->squash) {
        if (GST_BUFFER_DURATION_IS_VALID (inbuf)) {
          filter->ts_offset += inbuf->duration;
        } else {
          GST_DEBUG ("Invalid buffer duration: ts_offset not updated");
        }
      }
      return GST_BASE_TRANSFORM_FLOW_DROPPED;
    }

  } else {
    //GST_ERROR ("\nVoice detected");
    filter->consecutive_silence_buffers = 0;
    filter->consecutive_silence_time = 0;
    
    
    if (GST_BUFFER_PTS_IS_VALID (inbuf)) {
		
		GstMapInfo inbuf_map;
		gsize buffer_size;
		guint8 *buffer;
		
		gst_buffer_map (inbuf, &inbuf_map, GST_MAP_READ); /* map the buffer */
		
		buffer_size = (gsize)inbuf_map.size; /* copy the buffer size */
		buffer = (guint8 *)g_malloc0_n(buffer_size, sizeof(gint8)); /* allocate memory for buffer data */
		memcpy(buffer, inbuf_map.data, buffer_size); /* copy the buffer data */		
		
		gst_buffer_unmap (inbuf, &inbuf_map); /* unmap the buffer */
		
		/* emit the signal with buffer */
		g_signal_emit (filter, gst_remove_silence_signals[SIGNAL_BUFFER], 0, buffer, (gint)buffer_size);
		
		/* calculate the total size of buffer */
		total_buf_emited += (buffer_size / sizeof (guint8));
		if (total_buf_emited > filter->max_voice_buffer_size + 5000) {     
			SendMaxVoiceMessage(filter);
			GST_DEBUG ("MAX_Voice 2");
    	}
    }
	    
    if (filter->silence_detected) {
        if (!filter->silent) {
          if (GST_BUFFER_PTS_IS_VALID (inbuf)) {
            GstStructure *s;
            GstMessage *m;

            s = gst_structure_new ("removesilence", "silence_finished",
                G_TYPE_UINT64, GST_BUFFER_PTS (inbuf) - filter->ts_offset, NULL);

            m = gst_message_new_element (GST_OBJECT (filter), s);
            gst_element_post_message (GST_ELEMENT (filter), m);
            //GST_ERROR ("silence_finished");
          }
        }
        filter->silence_detected = FALSE;
    }
  }

  if (filter->squash && filter->ts_offset > 0) {
    if (GST_BUFFER_PTS_IS_VALID (inbuf)) {
      inbuf = gst_buffer_make_writable (inbuf);
      GST_BUFFER_PTS (inbuf) -= filter->ts_offset;
    } else {
      GST_DEBUG ("Invalid buffer pts, update not possibile");
    }
  }

  return GST_FLOW_OK;
}

/*Plugin init functions*/
static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "removesilence", GST_RANK_NONE,
      gst_remove_silence_get_type ());
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    removesilence,
    "Removes silence from an audio stream",
    plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN);
