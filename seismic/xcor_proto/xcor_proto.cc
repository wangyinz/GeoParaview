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
#include "filter++.h"
/* This is a core program that will eventually be a generalized
time-domain cross correlation gizmo.  For now it simply does i/o 
and writes output that will be passed to matlab for processing.  
*/
void usage()
{
	cerr << "xcor_proto db [-s subset_expression]" << endl;
	exit(-1);
}
void output_data(ThreeComponentEnsemble *d, int component)
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
void rotate_ensemble(ThreeComponentEnsemble *d,double vp, double vs,
			bool afst)
{
	int i;
	string phase;
	SlownessVector uvec;
	double lat,lon,elev;
	SphericalCoordinate scor;
	double umag;
	try {

	for(i=0;i<(d->tcse.size());++i)
	{
		Hypocenter hypo(dynamic_cast<Metadata&>(d->tcse[i]));
		phase = d->tcse[i].get_string("assoc.phase");
		lat = d->tcse[i].get_double("site.lat");
		lon = d->tcse[i].get_double("site.lon");
		elev = d->tcse[i].get_double("site.elev");
		uvec = hypo.phaseslow(lat,lon,elev,phase);

		if(afst)
			d->tcse[i].free_surface_transformation(uvec,vp,vs);
		else
		{
			scor = PMHalfspaceModel(vp,vs,uvec.ux,uvec.uy);
			d->tcse[i].rotate(scor);
		}
	}
	} catch (...) { throw; }
}
	

int main(int argc, char **argv)
{
	string pffile("xcor_proto");
	Dbptr db;
	Pf *pf;
	ThreeComponentEnsemble *d;
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
		// AttributeMap am();
		// Need the strange pair below to get this to compile
		AttributeMap* amptr=new AttributeMap();
		AttributeMap& am=*amptr;  // default uses css3.0 namespace
		MetadataList md_to_input=pfget_mdlist(pf,"input_list");
		Metadata md(pf);
		double ts=md.get_double("window_start_time");
		double te=md.get_double("window_end_time");
		TimeWindow tw(ts,te);
		double vs=md.get_double("vs0");
		double vp=md.get_double("vp0");
		bool rotate_data=md.get_bool("rotate_data");
		bool afst=false;
		string gather_type=md.get_string("gather_type");
		if(rotate_data)
			afst=md.get_bool("apply_free_surface_transformation");
		int component=md.get_int("component_to_extract");
		string TTmethod,TTmodel;
		try {
			TTmethod=md.get_string("TTmethod");
			TTmodel=md.get_string("TTmodel");
		} catch (MetadataGetError mderr)
		{
			cerr << "TTmethod:TTmodel not defined.  using default\n"
				<< endl;
		}
		// correct to C convention
		--component;
		double target_samprate=md.get_double("target_samprate");
		double dtout=1.0/target_samprate;
		ResamplingDefinitions rd(pf);
		string filter_spec=md.get_string("filter");
		bool filter_data;
		Time_Invariant_Filter filter;
		if(filter_spec=="none")
			filter_data=false;
		else
		{
			filter_data=true;
			filter=Time_Invariant_Filter(filter_spec);
		}
		if(dbopen(const_cast<char *>(dbname.c_str()),"r",&db))
                      die(0,"dbopen failed on database %s",dbname.c_str());
		DatascopeHandle dbhi0(db,pf,tag);
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
			group_keys.push_back("gridid");
			group_keys.push_back("sta");
		}
		else
		{
			group_keys.push_back("evid");
		}
		DatascopeHandle dbhi(dbhi0);
		dbhi.group(group_keys);
		dbhi.rewind();
		for(int irec=0;irec<dbhi.number_tuples();++dbhi,++irec)
		{
			int arid;
			//AttributeMap amtest(string("test"));
			vector<ThreeComponentSeismogram>::iterator smember;
			ThreeComponentEnsemble cutdata;
			TimeSeries *comp;
			d=new ThreeComponentEnsemble(dynamic_cast<DatabaseHandle&>
				(dbhi),md_to_input,md_to_input,am);
				//(dbhi),md_to_input,md_to_input,amtest);
				//(dbhi),mdl1,mdl2,amtest);
				 
			for(i=0;i<d->tcse.size();++i)
			{
				if(!(d->tcse[i].live)) continue;
				ThreeComponentSeismogram tcs(d->tcse[i]);
				// Need to push these to the station metadata
				if(!(TTmethod.empty() || TTmethod.empty()))
				{
					tcs.put("TTmethod",TTmethod);
					tcs.put("TTmodel",TTmodel);
				}
				if(gather_type=="source")
				{
					arid = tcs.get_int("arrival.arid");
					cout << arid <<" ";
				}
				else
				{
					cout << tcs.get_string("sta") <<" ";
				}
				int nsout;
				for(j=0;j<3;++j)
				{
					comp=ExtractComponent(d->tcse[i],j);
					TimeSeries tsdec=ResampleTimeSeries(*comp,
						rd,dtout,false);

					// Need a different sized matrix to hold output
					if(j==0)
					{
						nsout = tsdec.s.size();
						tcs.ns=nsout;
						tcs.dt=tsdec.dt;
						tcs.t0=tsdec.t0;

						tcs.u=dmatrix(3,nsout);
					}
					dcopy(nsout,&(tsdec.s[0]),1,
							tcs.u.get_address(j,0),3);
				}
				tcs.dt=dtout;
				d->tcse[i]=tcs;
			}
			if(filter_data) Filter_Ensemble(*d,filter);
			
			cutdata = Arrival_Time_Reference(*d,akey,tw);
			if(rotate_data)
				rotate_ensemble(&cutdata,vp,vs,afst);
			cout << endl;
			output_data(&cutdata,component);
			
			delete d;
		}
	} catch (SeisppError serr)
	{
		serr.log_error();
		elog_die(0,"Exit from seispp error\n");
	}
	catch (MetadataError merr)
	{
		merr.log_error();
	}
}
