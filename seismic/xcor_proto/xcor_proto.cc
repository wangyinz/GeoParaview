#include <string>
#include <math.h>
using namespace std;
#include "stock.h"
#include "coords.h"
#include "pf.h"
#include "tt.h"
#include "db.h"
#include "seispp.h"
using namespace SEISPP;
#include "resample.h"
#include "perf.h"
/* This is a core program that will eventually be a generalized
time-domain cross correlation gizmo.  For now it simply does i/o 
and writes output that will be passed to matlab for processing.  
*/
void usage()
{
	cerr << "xcor_proto db [-s subset_expression]" << endl;
	exit(-1);
}
void output_data(Three_Component_Ensemble *d, int component)
{
	int i,j;
	// assume all trace segments are the same size as the 
	// first one in the list.  Truncate or pad any that differ
	// That assumes such problems are marked as a gap
	int nsamples;
	nsamples = d->tcse[0].u.columns();
	// cout << nsamples << ' ' << d->tcse.size() << endl;
	for(j=0;j<nsamples;++j)
	{
		for(i=0;i<(d->tcse.size());++i)
		{
			if(d->tcse[i].is_gap(j))
			{
				cout << "0.0 ";
				cerr << "Warning:  gap at sample "
					<< j 
					<< " at emsemble member "
					<< i << endl;
			}
			else
			{
				if(j<d->tcse[i].u.columns() )
					cout << d->tcse[i].u(component,j)
						<< " ";
				else
					cout << "0.0 ";
			}
		}
		cout << endl;
	}
}
void rotate_ensemble(Three_Component_Ensemble *d,double vp, double vs,
			bool afst)
{
	double seaz,phi,delta;
	double depth;
	int i;
	string phase;
	Slowness_vector uvec;
	Spherical_Coordinate scor;
	double umag;

	for(i=0;i<(d->tcse.size());++i)
	{
		depth = d->tcse[i].get_double("origin.depth");
		phase = d->tcse[i].get_string("assoc.phase");
		seaz = d->tcse[i].get_double("assoc.seaz");
		delta = d->tcse[i].get_double("assoc.delta");

		umag = phase_slowness(const_cast<char *>(phase.c_str()),
				delta,depth);
		if(umag<0.0) throw seispp_error(string(
			"Computing slowness for phase"+phase
				+" failed") );
		// seaz is an azimuth (from north) and points toward
		// the source.  We need the opposite direction and 
		// as the angle relative to the x axis for a spherical
		// coordinate definition.
		phi = -(rad(seaz)+M_PI_2);
		uvec.ux = umag*cos(phi);
		uvec.uy = umag*sin(phi); 
		if(afst)
			d->tcse[i].free_surface_transformation(uvec,vp,vs);
		else
		{
			scor = pm_halfspace_model(vp,vs,uvec.ux,uvec.uy);
			d->tcse[i].rotate(scor);
		}
	}
}
	

int main(int argc, char **argv)
{
	string pffile("xcor_proto");
	Dbptr db;
	Pf *pf;
	Three_Component_Ensemble *d;
	int i,j;
	string testarg;
	string sstring;
	bool do_subset=false;

	ios::sync_with_stdio();
	elog_init(argc,argv);

	if(argc<2) usage();
	for(i=2;i<argc;++i)
	{
		testarg=string(argv[i]);
		if(testarg=="-s")
		{
			++i;
			if(i>=argc) usage();
			sstring=string(argv[i]);
			do_subset=true;
		}
		else
		{
			usage();
		}
	}

	try
	{
		string dbname(argv[1]);
		string tag("dbprocess_commands");
		string akey("arrival.time");
		if(pfread(const_cast<char *>(pffile.c_str()),&pf))
                      die(0,"Failure reading parameter file %s\n",pffile.c_str());
		// For some bizarre reason the following does not work:
		// Attribute_Map am();
		// Need the strange pair below to get this to compile
		Attribute_Map* amptr=new Attribute_Map();
		Attribute_Map& am=*amptr;  // default uses css3.0 namespace
		Metadata_list md_to_input=pfget_mdlist(pf,"input_metadata_list");
		Metadata md(pf);
		double ts=md.get_double("window_start_time");
		double te=md.get_double("window_end_time");
		Time_Window tw(ts,te);
		double vs=md.get_double("vs0");
		double vp=md.get_double("vp0");
		bool rotate_data=md.get_bool("rotate_data");
		bool afst=false;
		string gather_type=md.get_string("gather_type");
		if(rotate_data)
			afst=md.get_bool("apply_free_surface_transformation");
		int component=md.get_int("component_to_extract");
		// correct to C convention
		--component;
		double target_samprate=md.get_double("target_samprate");
		double dtout=1.0/target_samprate;
		Resampling_Definitions rd(pf);


		if(dbopen(const_cast<char *>(dbname.c_str()),"r",&db))
                      die(0,"dbopen failed on database %s",dbname.c_str());
		Datascope_Handle dbhi0(db,pf,tag);
		// Needs attention eventually.  Problem in current
		// Database handle implementation
		// if(do_subset) dbhi.subset(sstring);
cerr << "View size = "<<dbhi0.number_tuples() << endl;
		// This is necessary because dbprocess has no way of 
		// creating  handle that works for nested groups.  Nexted
		// groups are required to define a 3c ensemble.
		list<string> group_keys;
		if(gather_type=="source")
		{
			group_keys.push_back("cluster.gridid");
		}
		else
		{
			group_keys.push_back("evid");
		}
		Datascope_Handle dbhi=dbhi0;
		dbhi.group(group_keys);
		dbhi.rewind();
		for(int irec=0;irec<dbhi.number_tuples();++dbhi,++irec)
		{
			//Attribute_Map amtest(string("test"));
			vector<Three_Component_Seismogram>::iterator smember;
			Three_Component_Ensemble cutdata;
			Time_Series *comp;
			d=new Three_Component_Ensemble(dynamic_cast<Database_Handle&>
				(dbhi),md_to_input,md_to_input,am);
				//(dbhi),md_to_input,md_to_input,amtest);
				//(dbhi),mdl1,mdl2,amtest);
			for(i=0;i<d->tcse.size();++i)
			{
				Three_Component_Seismogram tcs(d->tcse[i]);
				cout << tcs.get_string("sta") 
					<< " has dt " 
					<< tcs.dt << endl;
				int nsout;
				for(j=0;j<3;++j)
				{
					comp=Extract_Component(d->tcse[i],j);
					Time_Series tsdec=Resample_Time_Series(*comp,
						rd,dtout,false);

					// Need a different sized matrix to hold output
					if(j==0)
					{
						nsout = tsdec.s.size();
						tcs.u=dmatrix(3,nsout);
					}
cerr << i << "," << j << "ns="<<d->tcse[i].ns<<" nsout="<<nsout<<endl;
cerr << "t0in="<<d->tcse[i].t0<<" t0resampled="<< tsdec.t0<<endl;
					dcopy(nsout,&(tsdec.s[0]),1,
							tcs.u.get_address(j,0),3);
				}
				tcs.dt=dtout;
				d->tcse[i]=tcs;
			}
			
			cutdata = Arrival_Time_Reference(*d,akey,tw);
			if(rotate_data)
				rotate_ensemble(&cutdata,vp,vs,afst);
			output_data(&cutdata,component);
			
			delete d;
		}
	} catch (seispp_error serr)
	{
		serr.log_error();
	}
	catch (Metadata_error merr)
	{
		merr.log_error();
	}
}
